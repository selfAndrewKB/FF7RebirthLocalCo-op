#include "player_two/vending.h"

#include "facts/controller.h"
#include "facts/party.h"
#include "player_two/field_rules.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/party_member.h"
#include "players/unreal_access.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <initializer_list>
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
// 1.005 (RE-1005-field-2.md §4): the camera's entry is at 0x14327FE5C, %16 = 12, so its first instruction crosses a
// 16-byte block and cannot take an entry redirect. Its seven callers (the same seven shop paths as in this build, one
// call each, each inside one block in 1.005) are redirected instead, to the same replacement.
constexpr std::array<std::uintptr_t, 7> kCameraCalls{
    0x1435387D8,  // sub_143538398
    0x14353D73F,  // sub_14353D5AC
    0x14353FA6F,  // sub_14353F7F4
    0x14354EE92,  // FEndShowChadleyShopMenu_Begin
    0x14354F409,  // sub_14354F1F0
    0x14354FD23,  // FEndShowShopMenu_Begin (the vending path)
    0x1435502E2,  // FEndShowSkillTreeMenu_Begin
};
constexpr std::size_t kChadleyCall = 3;  // kCameraCalls' FEndShowChadleyShopMenu_Begin
using Clock = std::chrono::steady_clock;
// Player 2's press claims the shop whose camera comes within this many frames (the level script opens it the next frame,
// §15.1) and names the pressed machine (the camera's target is the machine's Tags[0], which the pressed point's key
// contains, e.g. VendingMachine_..._obl2250_VendingBook_NearDrawbridge_PressVolume). Any other shop stays player 1's.
constexpr std::uint64_t kPressFrames = 60;
constexpr auto          kPressWindow = std::chrono::seconds(5);  // a press older than this is logged as having opened no shop
// A shopkeeper's shop opens after its greeting, if any (a voiced line of about 2 s and more; RE-p2-shopkeeper-talk.md
// §4.1): player 2's press of a shopkeeper claims the first shop camera within this time that names the pressed NPC.
constexpr auto          kShopkeeperWindow = std::chrono::seconds(15);
// A shop up holds a pending press's window (player 2's shop may be queued behind it) for at most this long, in case its
// close is never seen (a map change mid-shop).
constexpr auto          kHoldAtMost = std::chrono::seconds(120);

// Each wrapped function takes at most four integer arguments (§15 listings); all four are passed on.
using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

platform::EntryRedirect g_end2Key, g_camera, g_cameraOff;
// The shop camera's caller in FEndShowChadleyShopMenu_Begin (its return address, this build): Chadley's shop keeps its
// camera for player 2, since coming back from a VR battle activates the shop camera again outside the hooked function with
// the target and preset saved at the VR start (RE-p2-shop-camera-skip.md §7).
std::uintptr_t g_chadleyReturn = 0;
std::array<platform::CallRedirect, kCameraCalls.size()> g_cameraCalls;  // 1.005: the camera's callers
std::uintptr_t          g_cameraTarget = 0;  // 1.005: the camera function (the callers' target)
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
Press         g_shopkeeper;  // player 2's press of a shopkeeper (player_two/shop_talk); point = the NPC's CharaPop key
Shop          g_shop = Shop::None;
Clock::time_point g_shopAt{};  // when the shop up now opened
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

/// The text after the first '_', lower case (the NPC's ActorName and its CharaPop key share it: Npc2100_shopweapon01
/// and oba2100_ShopWeapon01; RE-p2-shopkeeper-talk.md §7.3).
std::wstring AfterPrefix(const std::wstring& name)
{
    return field_rules::AfterPrefix(name);
}

/// Whether the shop camera's target names the NPC of player 2's pending shopkeeper press (the action passes the NPC's
/// ActorName, §4.1). No target is never a shopkeeper's: some vending scripts pass none, and player 1's machine must not
/// take player 2's pending press.
bool NamesPressedShopkeeper(std::uint64_t target) noexcept
{
    if (target == 0)
    {
        return false;
    }
    try
    {
        const std::wstring rest = AfterPrefix(players::unreal::NameText(target));
        return !rest.empty() && rest == AfterPrefix(players::unreal::NameText(g_shopkeeper.point));
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
    const auto point  = ReadAt<std::uint64_t>(key);
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
/// Player 2's shop opens without it (the user, 2026-10-18: "When P2 initiates them, just disable the entire scripted
/// camera zoom action on P1's view"): the camera would run on player 1's camera manager, framing player 1's character
/// and leaving player 1's camera at the shop's angle on close (RE-p2-shop-camera-skip.md). Nothing in the shop waits on
/// it and no caller reads its result (§2, §3); its deactivation at the close still runs and does nothing (§4).
std::uint64_t Camera(std::uint64_t target, std::uint64_t preset, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto caller = CallerOf(_ReturnAddress());
    const auto call   = reinterpret_cast<Fn>(g_cameraTarget != 0 ? g_cameraTarget : g_camera.Original());
    if (!generation::OnGameThread() || !generation::Active())
    {
        return call(target, preset, r8, r9);
    }
    const auto frames  = platform::frame_watch::FrameIndex() - g_press.frame;
    const bool machine = g_press.active && frames <= kPressFrames && NamesPressedMachine(target);
    if (g_press.active && !machine)
    {
        platform::Log(L"vending: player 2's press of {} is not this shop's ({} frames since it, camera target {})",
                      players::unreal::NameText(g_press.point), frames, players::unreal::NameText(target));
    }
    // A press stays pending through another shop (player 1's opened first, and the game queues player 2's behind it:
    // the event runner waits while a shop's Exclusion bit is live, RE-p2-shopkeeper-talk.md §6) until its own shop names
    // what was pressed; its window is paused while another shop is up and restarts when that shop closes (CameraOff).
    if (machine)
    {
        g_press.active = false;
    }
    else if (g_press.active && frames > kPressFrames)
    {
        g_press.active = false;  // its shop would have come within the frames since the press (or the last close)
    }
    const bool keeper = !machine && g_shopkeeper.active && NamesPressedShopkeeper(target);
    if (g_shopkeeper.active)
    {
        platform::Log(L"vending: player 2's press of shopkeeper {} {} ({:.1f} s, {} frames since it, camera target {})",
                      players::unreal::NameText(g_shopkeeper.point), keeper ? L"opened this shop" : L"is not this shop's",
                      std::chrono::duration<double>(Clock::now() - g_shopkeeper.at).count(),
                      platform::frame_watch::FrameIndex() - g_shopkeeper.frame, players::unreal::NameText(target));
    }
    if (keeper)
    {
        g_shopkeeper.active = false;
    }
    const bool twos = machine || keeper;
    g_shopAt        = Clock::now();
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
    const bool chadley = g_chadleyReturn != 0 && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_chadleyReturn;
    if (!chadley)
    {
        platform::Log(L"vending: player 2's shop opened (camera target {}, preset {}, from 0x{:X}); pad 2 drives it; the "
                      L"shop camera is skipped (player 1's view stays as it is)",
                      players::unreal::NameText(target), players::unreal::NameText(preset), caller);
        return 0;  // no caller reads it (RE-p2-shop-camera-skip.md §2)
    }
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
    platform::Log(L"vending: player 2's shop opened (Chadley's: its camera runs, target {}, preset {} by {}, from 0x{:X}); "
                  L"pad 2 drives it",
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
    // A press of player 2's queued behind this shop: its window starts again now (the game opens the queued shop next).
    const auto now   = Clock::now();
    const auto frame = platform::frame_watch::FrameIndex();
    for (Press* press : {&g_press, &g_shopkeeper})
    {
        if (press->active)
        {
            press->at    = now;
            press->frame = frame;
            platform::Log(L"vending: player 2's press of {} still waits for its shop; its window starts again",
                          players::unreal::NameText(press->point));
        }
    }
    return result;
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, std::uintptr_t address,
           std::span<const std::uint8_t> bytes, Fn replacement) noexcept
{
    const auto at = image.Rebase(address);
    return at && image.BytesMatch(address, bytes) && redirect.Install(*at, image.EntryInstruction(address, bytes), reinterpret_cast<std::uintptr_t>(replacement));
}

/// 1.005: the shop camera's activation at its seven callers (all or none: a shop path left out would open player 2's
/// shop as player 1's).
bool CameraCalls(const platform::GameImage& image) noexcept
{
    const auto target = image.Rebase(kCamera);
    if (!target || !image.BytesMatch(kCamera, kCameraBytes))
    {
        return false;
    }
    g_cameraTarget = *target;  // before the first install: the replacement runs the moment it is reachable
    bool all       = true;
    for (std::size_t i = 0; i < kCameraCalls.size() && all; ++i)
    {
        const auto site = image.Rebase(kCameraCalls[i], 5);
        all = site && g_cameraCalls[i].Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Camera));
    }
    if (!all)
    {
        for (auto& call : g_cameraCalls)
        {
            if (call.IsInstalled())
            {
                call.Restore();
            }
        }
    }
    return all;
}

}  // namespace

bool Admits(std::uint64_t pointPreset) noexcept
{
    return pointPreset != 0 && g_shop == Shop::None && !g_shopkeeper.active && pointPreset == PresetName();
}

void PressedShopkeeper(std::uint64_t key) noexcept
{
    g_shopkeeper = Press{true, Clock::now(), platform::frame_watch::FrameIndex(), key};
}

bool ShopIdle() noexcept
{
    return g_shop == Shop::None && !g_press.active && !g_shopkeeper.active;
}

bool ShopkeeperPending() noexcept
{
    return g_shopkeeper.active;
}

bool PlayerTwosShop() noexcept
{
    return g_shop == Shop::PlayerTwo;
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    // A shop up holds the windows (a press of player 2's may be queued behind it; CameraOff restarts them).
    const bool held = g_shop != Shop::None && generation::Active() && Clock::now() - g_shopAt < kHoldAtMost;
    if (g_press.active && !held && Clock::now() - g_press.at > kPressWindow)
    {
        g_press.active = false;
        platform::Log(L"vending: player 2's press of {} opened no shop within {} s", players::unreal::NameText(g_press.point),
                      std::chrono::duration_cast<std::chrono::seconds>(kPressWindow).count());
    }
    if (g_shopkeeper.active && !generation::Active())
    {
        g_shopkeeper.active = false;
        platform::Log(L"vending: player 2 left with their press of shopkeeper {} pending", players::unreal::NameText(g_shopkeeper.point));
    }
    else if (g_shopkeeper.active && !held && Clock::now() - g_shopkeeper.at > kShopkeeperWindow)
    {
        g_shopkeeper.active = false;
        platform::Log(L"vending: player 2's press of shopkeeper {} opened no shop within {} s",
                      players::unreal::NameText(g_shopkeeper.point),
                      std::chrono::duration_cast<std::chrono::seconds>(kShopkeeperWindow).count());
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
    const auto chadley = image.Rebase(kCameraCalls[kChadleyCall], 5);
    g_chadleyReturn    = chadley ? *chadley + 5 : 0;  // the call is 5 bytes (E8 rel32)
    const bool presses = g_partyManager != 0 && Entry(image, g_end2Key, kEnd2Key, kEnd2KeyBytes, &End2Key);
    const bool cameras = platform::RunningBuild() == platform::BuildIndex::V1005
                             ? CameraCalls(image)
                             : Entry(image, g_camera, kCamera, kCameraBytes, &Camera);
    const bool closes  = Entry(image, g_cameraOff, kCameraOff, kCameraOffBytes, &CameraOff);
    const bool ready   = g_vendingPreset != nullptr && presses && cameras && closes;
    platform::Log(L"vending: {} (presses {}, the shop camera's activation {}, its deactivation {}; Chadley's camera call {})",
                  ready ? L"ready (player 2 uses vending machines through the game's own field action and shop; the pad of "
                          L"whoever opened the shop drives it; player 2's shops open without the shop camera)"
                        : L"UNAVAILABLE for player 2 (a check failed; machines stay player 1's)",
                  presses ? L"watched" : L"NOT watched", cameras ? L"watched" : L"NOT watched",
                  closes ? L"watched" : L"NOT watched", g_chadleyReturn != 0 ? L"found" : L"NOT found (its camera is skipped too)");
    if (!ready)
    {
        g_vendingPreset = nullptr;  // Admits says no: machines stay player 1's
    }
}

}  // namespace lss::player_two::vending
