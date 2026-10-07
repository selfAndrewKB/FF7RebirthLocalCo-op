#include "player_two/vending.h"

#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/party_member.h"
#include "players/unreal_access.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::vending {

namespace {

// RE-p2-field-interact.md §15.9 (this build).
constexpr std::uintptr_t kVendingPreset     = 0x149347278;  // the FName global rpaVendingMachine (the point-side preset)
constexpr std::uintptr_t kEnd2Key           = 0x1414A8A94;  // FieldActionKeyListener_End2FieldActionActor_CallVt6A0(mgr, &key)
constexpr std::uint8_t   kEnd2KeyBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx
constexpr std::uintptr_t kCamera            = 0x143550CC4;  // ShopMenu_ActivateShopCamera_TargetOrSlot0Key(FName target, FName preset)
constexpr std::uint8_t   kCameraBytes[]     = {0x48, 0x89, 0x5C, 0x24, 0x20};  // mov [rsp+20h], rbx
constexpr std::uintptr_t kCameraOff         = 0x143550C90;  // ShopMenu_DeactivateShopCamera
constexpr std::uint8_t   kCameraOffBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00};  // mov eax, 28h
using Clock = std::chrono::steady_clock;
// Player 2's press claims the shop whose camera comes within this many frames (the level script opens it the next frame,
// §15.1) and names the pressed machine (the camera's target is the machine's Tags[0], which the pressed point's key
// contains, e.g. VendingMachine_..._obl2250_VendingBook_NearDrawbridge_PressVolume). Any other shop stays player 1's.
constexpr std::uint64_t kPressFrames = 60;
constexpr auto          kPressWindow = std::chrono::seconds(5);  // a press older than this is logged as having opened no shop

// Each wrapped function takes at most four integer arguments (§15 listings); all four are passed on.
using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

platform::EntryRedirect g_end2Key, g_camera, g_cameraOff;
const std::uint64_t*    g_vendingPreset = nullptr;  // the FName global's live address
std::uintptr_t          g_partyManager  = 0;        // the pointer global's live address
std::optional<platform::GameImage> g_image;  // for research IDA addresses in the log
constexpr std::uint32_t kPartyLeader    = 0x148;    // the party manager's leader, an FWeakObjectPtr

// Game thread.
enum class Shop
{
    None,
    PlayerOne,
    PlayerTwo,
};

/// Player 2's press that should open a shop.
struct Press
{
    bool              active = false;
    Clock::time_point at{};
    std::uint64_t     frame = 0;
    std::uint64_t     point = 0;  // the pressed point's key
};
Press         g_press;
Shop          g_shop = Shop::None;
float         g_memberAt[3]{};  // player 2's member when player 2's shop opened
bool          g_memberKnown = false;
std::uint64_t g_presetName  = 0;  // the FName, once read (set by the game's static init)

std::uint64_t PresetName() noexcept
{
    if (g_presetName == 0 && g_vendingPreset != nullptr)
    {
        platform::CopyGuarded(&g_presetName, g_vendingPreset, sizeof(g_presetName));
    }
    return g_presetName;
}

std::uint64_t KeyAt(std::uint64_t pointer) noexcept
{
    return pointer != 0 ? ReadField<std::uint64_t>(reinterpret_cast<const void*>(pointer), 0) : 0;
}

/// The caller's IDA address (its return address), for the log.
std::uintptr_t CallerOf(const void* returnAddress) noexcept
{
    const auto at = reinterpret_cast<std::uintptr_t>(returnAddress);
    return g_image ? g_image->ResearchAddress(at) : at;
}

const wchar_t* Whose(const void* character) noexcept
{
    if (character == nullptr)
    {
        return L"nobody";
    }
    if (character == generation::Member())
    {
        return L"player 2's member";
    }
    if (character == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return L"another character";
}

const void* Leader() noexcept
{
    const void* manager = ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0);
    return manager != nullptr ? leader_scope::ObjectAt(static_cast<const std::uint8_t*>(manager) + kPartyLeader) : nullptr;
}

/// Whether the shop camera's target names the machine of player 2's pending press (no target: a press is enough).
bool NamesPressedMachine(std::uint64_t target) noexcept
{
    if (target == 0)
    {
        return true;
    }
    try
    {
        const std::wstring point = players::unreal::NameText(g_press.point);
        const std::wstring name  = players::unreal::NameText(target);
        return !name.empty() && point.find(name) != std::wstring::npos;
    }
    catch (...)
    {
        return false;
    }
}

/// The End2 listener (the vending press path, and other End2 actors'): logged; player 2's successful press is kept for the
/// shop it opens.
std::uint64_t End2Key(std::uint64_t mgr, std::uint64_t key, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_end2Key.Original())(mgr, key, r8, r9);
    if (!generation::OnGameThread() || !generation::Active() || (result & 0xFF) == 0)
    {
        return result;
    }
    const bool inPass = field_pickup::InPass();
    const auto point  = KeyAt(key);
    if (inPass)  // player 1's presses (any End2 action) leave player 2's pending press alone
    {
        g_press = Press{true, Clock::now(), platform::frame_watch::FrameIndex(), point};
    }
    platform::Log(L"vending: frame {}: field action {} pressed {}, leader {}", platform::frame_watch::FrameIndex(),
                  players::unreal::NameText(point), inPass ? L"in player 2's pass" : L"in player 1's (the game's own) run",
                  Whose(Leader()));
    return result;
}

/// The shop camera's activation (every shop path; the shop UI and OnOff 148 are already up): whose shop this is.
std::uint64_t Camera(std::uint64_t target, std::uint64_t preset, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto caller = CallerOf(_ReturnAddress());
    const auto call   = reinterpret_cast<Fn>(g_camera.Original());
    if (!generation::OnGameThread() || !generation::Active())
    {
        return call(target, preset, r8, r9);
    }
    const auto frames = platform::frame_watch::FrameIndex() - g_press.frame;
    const bool twos   = g_press.active && frames <= kPressFrames && NamesPressedMachine(target);
    if (g_press.active && !twos)
    {
        platform::Log(L"vending: player 2's press of {} is not this shop's ({} frames since it, camera target {})",
                      players::unreal::NameText(g_press.point), frames, players::unreal::NameText(target));
    }
    g_press.active = false;
    if (!twos)
    {
        g_shop = Shop::PlayerOne;
        platform::Log(L"vending: player 1's shop opened (camera target {}, preset {}, from 0x{:X})",
                      players::unreal::NameText(target), players::unreal::NameText(preset), caller);
        return call(target, preset, r8, r9);
    }
    g_shop            = Shop::PlayerTwo;
    void*      member = generation::Member();
    const auto type   = party_member::PlayerTypeOf(member);
    g_memberKnown     = member != nullptr && charapop::ActorLocation(member, g_memberAt);
    pad_buttons::SetMenuPlayer(1);
    std::uint64_t result = 0;
    if (type)
    {
        const leader_scope::LeaderTypeScope scope(*type);  // the default preset by player 2's member's type (§15.3)
        result = call(target, preset, r8, r9);
    }
    else
    {
        result = call(target, preset, r8, r9);
    }
    platform::Log(L"vending: player 2's shop opened (camera target {}, preset {} by {}, from 0x{:X}); pad 2 drives it",
                  players::unreal::NameText(target), players::unreal::NameText(preset),
                  type ? L"player 2's member's type" : L"player 1's type (no member type)", caller);
    return result;
}

/// The shop camera's deactivation (every shop path's close).
std::uint64_t CameraOff(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_cameraOff.Original())(a, b, c, d);
    if (!generation::OnGameThread())
    {
        return result;
    }
    if (g_shop == Shop::PlayerTwo)
    {
        pad_buttons::SetMenuPlayer(0);
        float      now[3]{};
        void*      member = generation::Member();
        const bool known  = g_memberKnown && member != nullptr && charapop::ActorLocation(member, now);
        platform::Log(L"vending: player 2's shop closed; pad 1 drives the menus again; player 2's member moved {:.1f} m while "
                      L"it was up",
                      known ? charapop::Distance(now, g_memberAt) / 100.0f : -1.0f);
    }
    else if (g_shop == Shop::PlayerOne && generation::Active())
    {
        platform::Log(L"vending: player 1's shop closed");
    }
    g_shop = Shop::None;
    return result;
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, std::uintptr_t address,
           std::span<const std::uint8_t> bytes, Fn replacement) noexcept
{
    const auto at = image.Rebase(address);
    return at && image.BytesMatch(address, bytes) && redirect.Install(*at, image.EntryInstruction(address, bytes), reinterpret_cast<std::uintptr_t>(replacement));
}

}  // namespace

bool Admits(std::uint64_t pointPreset) noexcept
{
    return pointPreset != 0 && g_shop == Shop::None && pointPreset == PresetName();
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (g_press.active && Clock::now() - g_press.at > kPressWindow)
    {
        g_press.active = false;
        platform::Log(L"vending: player 2's press of {} opened no shop within {} s", players::unreal::NameText(g_press.point),
                      std::chrono::duration_cast<std::chrono::seconds>(kPressWindow).count());
    }
    if (g_shop == Shop::PlayerTwo && !generation::Active())
    {
        g_shop = Shop::None;  // player 2 left with their shop up: the one pad again
        pad_buttons::SetMenuPlayer(0);
        platform::Log(L"vending: player 2 left while their shop was up; pad 1 drives the menus again");
    }
}

void Install(const platform::GameImage& image) noexcept
{
    const auto preset  = image.Rebase(kVendingPreset, sizeof(std::uint64_t));
    g_vendingPreset    = preset ? reinterpret_cast<const std::uint64_t*>(*preset) : nullptr;
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    g_partyManager     = manager ? *manager : 0;
    g_image            = image;
    const bool presses = g_partyManager != 0 && Entry(image, g_end2Key, kEnd2Key, kEnd2KeyBytes, &End2Key);
    const bool cameras = Entry(image, g_camera, kCamera, kCameraBytes, &Camera);
    const bool closes  = Entry(image, g_cameraOff, kCameraOff, kCameraOffBytes, &CameraOff);
    const bool ready   = g_vendingPreset != nullptr && presses && cameras && closes;
    platform::Log(L"vending: {} (presses {}, the shop camera's activation {}, its deactivation {})",
                  ready ? L"ready (player 2 uses vending machines through the game's own field action and shop; the pad of "
                          L"whoever opened the shop drives it)"
                        : L"UNAVAILABLE for player 2 (a check failed; machines stay player 1's)",
                  presses ? L"watched" : L"NOT watched", cameras ? L"watched" : L"NOT watched",
                  closes ? L"watched" : L"NOT watched");
    if (!ready)
    {
        g_vendingPreset = nullptr;  // Admits says no: machines stay player 1's
    }
}

}  // namespace lss::player_two::vending
