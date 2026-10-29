#include "world/vending.h"

#include "facts/controller.h"
#include "facts/party.h"
#include "input/pad_buttons.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"
#include "session/party_member.h"
#include "world/charapop_facts.h"
#include "world/field_pickup.h"
#include "world/field_rules.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::world::vending {

namespace {

// RE-p2-field-interact.md §15.9 (this build).
constexpr std::uintptr_t kVendingPreset     = 0x149347278;  // the FName global rpaVendingMachine (the point-side preset)
constexpr std::uint8_t   kEnd2KeyBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx
// FieldActionKeyListener_End2FieldActionActor_CallVt6A0(mgr, &key)
constexpr platform::FunctionFact kEnd2Key{L"FieldActionKeyListener_End2FieldActionActor_CallVt6A0", 0x1414A8A94,
                                          kEnd2KeyBytes};
constexpr std::uint8_t   kCameraBytes[]     = {0x48, 0x89, 0x5C, 0x24, 0x20};  // mov [rsp+20h], rbx
// ShopMenu_ActivateShopCamera_TargetOrSlot0Key(FName target, FName preset)
constexpr platform::FunctionFact kCamera{L"ShopMenu_ActivateShopCamera_TargetOrSlot0Key", 0x143550CC4, kCameraBytes};
constexpr std::uint8_t   kCameraOffBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00};  // mov eax, 28h
// ShopMenu_DeactivateShopCamera
constexpr platform::FunctionFact kCameraOff{L"ShopMenu_DeactivateShopCamera", 0x143550C90, kCameraOffBytes};
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
using facts::kPartyManagerLeader;

// Game thread.

/// Whose shop is up: none, player 1's, or a seat's (one shop at a time, the game has one).
constexpr int kNoShop = -1;

/// A seat's press that should open a shop.
struct Press
{
    bool              active = false;
    Clock::time_point at{};
    std::uint64_t     frame = 0;
    std::uint64_t     point = 0;  // the pressed point's key
};
// By seat (players/seats.h; index 0, player 1's, unused: player 1's presses open player 1's shops as the game does).
Press         g_press[players::kMaxPlayers];
Press         g_shopkeeper[players::kMaxPlayers];  // a seat's press of a shopkeeper (shop_talk); point = its CharaPop key
int           g_shop = kNoShop;  // whose shop is up (0 player 1's, a seat's), or kNoShop
Clock::time_point g_shopAt{};  // when the shop up now opened
float         g_memberAt[3]{};  // the owning seat's member when its shop opened
bool          g_memberKnown = false;
std::uint64_t g_presetName  = 0;  // the FName, once read (set by the game's static init)

bool AnyShopkeeperPending() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_shopkeeper[seat].active)
        {
            return true;
        }
    }
    return false;
}

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
    constexpr const wchar_t* kMembers[players::kMaxPlayers] = {L"player 1's member", L"player 2's member",
                                                               L"player 3's member"};
    if (character == nullptr)
    {
        return L"nobody";
    }
    const int seat = players::generation::SeatOfMember(character);
    if (players::SeatBuilt(seat))
    {
        return kMembers[seat];
    }
    if (character == ReadPointer(players::generation::PlayerOneController(), facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return L"another character";
}

const void* Leader() noexcept
{
    const void* manager = ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0);
    return manager != nullptr ? scopes::leader_scope::ObjectAt(static_cast<const std::uint8_t*>(manager) + kPartyManagerLeader)
                              : nullptr;
}

/// Whether the shop camera's target names the machine of `press` (no target: a press is enough).
bool NamesPressedMachine(const Press& press, std::uint64_t target) noexcept
{
    if (target == 0)
    {
        return true;
    }
    try
    {
        const std::wstring point = players::unreal::NameText(press.point);
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

/// Whether the shop camera's target names the NPC of the pending shopkeeper `press` (the action passes the NPC's
/// ActorName, §4.1). No target is never a shopkeeper's: some vending scripts pass none, and player 1's machine must not
/// take a seat's pending press.
bool NamesPressedShopkeeper(const Press& press, std::uint64_t target) noexcept
{
    if (target == 0)
    {
        return false;
    }
    try
    {
        const std::wstring rest = AfterPrefix(players::unreal::NameText(target));
        return !rest.empty() && rest == AfterPrefix(players::unreal::NameText(press.point));
    }
    catch (...)
    {
        return false;
    }
}

/// The End2 listener (the vending press path, and other End2 actors'): logged; a seat's successful press, in its pass,
/// is kept for the shop it opens.
std::uint64_t End2Key(std::uint64_t mgr, std::uint64_t key, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_end2Key.Original())(mgr, key, r8, r9);
    if (!players::generation::OnGameThread() || !players::generation::AnyActive() || (result & 0xFF) == 0)
    {
        return result;
    }
    const int  seat   = field_pickup::PassSeat();
    const bool inPass = players::SeatBuilt(seat);
    const auto point  = ReadAt<std::uint64_t>(key);
    if (inPass)  // player 1's presses (any End2 action) leave the seats' pending presses alone
    {
        g_press[seat] = Press{true, Clock::now(), platform::frame_watch::FrameIndex(), point};
    }
    try
    {
        platform::Log(L"vending: frame {}: field action {} pressed {}, leader {}", platform::frame_watch::FrameIndex(),
                      players::unreal::NameText(point),
                      inPass ? std::format(L"in {}'s pass", players::SeatName(seat))
                             : std::wstring(L"in player 1's (the game's own) run"),
                      Whose(Leader()));
    }
    catch (...)
    {
    }
    return result;
}

/// The shop camera's activation (every shop path; the shop UI and OnOff 148 are already up): whose shop this is.
/// A seat's shop opens without it (the user, 2026-10-18: "When P2 initiates them, just disable the entire scripted
/// camera zoom action on P1's view"): the camera would run on player 1's camera manager, framing player 1's character
/// and leaving player 1's camera at the shop's angle on close (RE-p2-shop-camera-skip.md). Nothing in the shop waits on
/// it and no caller reads its result (§2, §3); its deactivation at the close still runs and does nothing (§4). The
/// seats' pending presses are matched in seat order: machines first, then shopkeepers.
std::uint64_t Camera(std::uint64_t target, std::uint64_t preset, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto caller = CallerOf(_ReturnAddress());
    const auto call   = reinterpret_cast<Fn>(g_cameraTarget != 0 ? g_cameraTarget : g_camera.Original());
    if (!players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        return call(target, preset, r8, r9);
    }
    int owner = kNoShop;  // the seat whose press opened this shop
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Press&     press   = g_press[seat];
        const auto frames  = platform::frame_watch::FrameIndex() - press.frame;
        const bool machine = owner == kNoShop && press.active && frames <= kPressFrames && NamesPressedMachine(press, target);
        if (press.active && !machine)
        {
            platform::Log(L"vending: {}'s press of {} is not this shop's ({} frames since it, camera target {})",
                          players::SeatName(seat), players::unreal::NameText(press.point), frames,
                          players::unreal::NameText(target));
        }
        // A press stays pending through another shop (player 1's opened first, and the game queues the seat's behind
        // it: the event runner waits while a shop's Exclusion bit is live, RE-p2-shopkeeper-talk.md §6) until its own
        // shop names what was pressed; its window is paused while another shop is up and restarts when that shop closes
        // (CameraOff).
        if (machine)
        {
            press.active = false;
            owner        = seat;
        }
        else if (press.active && frames > kPressFrames)
        {
            press.active = false;  // its shop would have come within the frames since the press (or the last close)
        }
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Press&     press  = g_shopkeeper[seat];
        const bool keeper = owner == kNoShop && press.active && NamesPressedShopkeeper(press, target);
        if (press.active)
        {
            platform::Log(L"vending: {}'s press of shopkeeper {} {} ({:.1f} s, {} frames since it, camera target {})",
                          players::SeatName(seat), players::unreal::NameText(press.point),
                          keeper ? L"opened this shop" : L"is not this shop's",
                          std::chrono::duration<double>(Clock::now() - press.at).count(),
                          platform::frame_watch::FrameIndex() - press.frame, players::unreal::NameText(target));
        }
        if (keeper)
        {
            press.active = false;
            owner        = seat;
        }
    }
    g_shopAt = Clock::now();
    if (owner == kNoShop)
    {
        g_shop = 0;
        platform::Log(L"vending: player 1's shop opened (camera target {}, preset {}, from 0x{:X})",
                      players::unreal::NameText(target), players::unreal::NameText(preset), caller);
        return call(target, preset, r8, r9);
    }
    // The menu pad by seat: pad_buttons numbers its players as the seats (0 player 1, then the seats).
    g_shop                   = owner;
    const wchar_t* const who = players::SeatName(owner);
    void*      member        = players::generation::MemberOf(owner);
    const auto type          = session::party_member::PlayerTypeOf(member);
    g_memberKnown            = member != nullptr && charapop::ActorLocation(member, g_memberAt);
    input::pad_buttons::SetMenuPlayer(owner);
    const bool chadley = g_chadleyReturn != 0 && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_chadleyReturn;
    if (!chadley)
    {
        platform::Log(L"vending: {}'s shop opened (camera target {}, preset {}, from 0x{:X}); pad {} drives it; the "
                      L"shop camera is skipped (player 1's view stays as it is)",
                      who, players::unreal::NameText(target), players::unreal::NameText(preset), caller, owner + 1);
        return 0;  // no caller reads it (RE-p2-shop-camera-skip.md §2)
    }
    std::uint64_t result = 0;
    if (type)
    {
        const scopes::leader_scope::LeaderTypeScope scope(*type);  // the default preset by the seat's member's type (§15.3)
        result = call(target, preset, r8, r9);
    }
    else
    {
        result = call(target, preset, r8, r9);
    }
    constexpr const wchar_t* kByType[players::kMaxPlayers] = {L"player 1's type", L"player 2's member's type",
                                                              L"player 3's member's type"};
    platform::Log(L"vending: {}'s shop opened (Chadley's: its camera runs, target {}, preset {} by {}, from 0x{:X}); "
                  L"pad {} drives it",
                  who, players::unreal::NameText(target), players::unreal::NameText(preset),
                  type ? kByType[owner] : L"player 1's type (no member type)", caller, owner + 1);
    return result;
}

/// The shop camera's deactivation (every shop path's close).
std::uint64_t CameraOff(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_cameraOff.Original())(a, b, c, d);
    if (!players::generation::OnGameThread())
    {
        return result;
    }
    if (players::SeatBuilt(g_shop))
    {
        input::pad_buttons::SetMenuPlayer(0);
        float      now[3]{};
        void*      member = players::generation::MemberOf(g_shop);
        const bool known  = g_memberKnown && member != nullptr && charapop::ActorLocation(member, now);
        platform::Log(L"vending: {}'s shop closed; pad 1 drives the menus again; {}'s member moved {:.1f} m while it was "
                      L"up",
                      players::SeatName(g_shop), players::SeatName(g_shop),
                      known ? charapop::Distance(now, g_memberAt) / 100.0f : -1.0f);
    }
    else if (g_shop == 0 && players::generation::AnyActive())
    {
        platform::Log(L"vending: player 1's shop closed");
    }
    g_shop = kNoShop;
    // A seat's press queued behind this shop: its window starts again now (the game opens the queued shop next).
    const auto now   = Clock::now();
    const auto frame = platform::frame_watch::FrameIndex();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        for (Press* press : {&g_press[seat], &g_shopkeeper[seat]})
        {
            if (press->active)
            {
                press->at    = now;
                press->frame = frame;
                platform::Log(L"vending: {}'s press of {} still waits for its shop; its window starts again",
                              players::SeatName(seat), players::unreal::NameText(press->point));
            }
        }
    }
    return result;
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
           Fn replacement) noexcept
{
    const auto at = platform::FindFunction(image, fact);
    return at && redirect.Install(*at, image.EntryInstruction(fact.address, fact.entryBytes),
                                  reinterpret_cast<std::uintptr_t>(replacement));
}

/// 1.005: the shop camera's activation at its seven callers (all or none: a shop path left out would open player 2's
/// shop as player 1's).
bool CameraCalls(const platform::GameImage& image) noexcept
{
    const auto target = platform::FindFunction(image, kCamera);
    if (!target)
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
    return pointPreset != 0 && g_shop == kNoShop && !AnyShopkeeperPending() && pointPreset == PresetName();
}

void PressedShopkeeper(int seat, std::uint64_t key) noexcept
{
    if (players::SeatBuilt(seat))
    {
        g_shopkeeper[seat] = Press{true, Clock::now(), platform::frame_watch::FrameIndex(), key};
    }
}

bool ShopIdle() noexcept
{
    if (g_shop != kNoShop)
    {
        return false;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_press[seat].active || g_shopkeeper[seat].active)
        {
            return false;
        }
    }
    return true;
}

bool ShopkeeperPending() noexcept
{
    return AnyShopkeeperPending();
}

bool ShopkeeperPending(int seat) noexcept
{
    return players::SeatBuilt(seat) && g_shopkeeper[seat].active;
}

int ShopSeat() noexcept
{
    return players::SeatBuilt(g_shop) ? g_shop : -1;
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    // A shop up holds the windows (a seat's press may be queued behind it; CameraOff restarts them).
    const bool held = g_shop != kNoShop && players::generation::AnyActive() && Clock::now() - g_shopAt < kHoldAtMost;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const wchar_t* const who        = players::SeatName(seat);
        Press&               press      = g_press[seat];
        Press&               shopkeeper = g_shopkeeper[seat];
        if (press.active && !held && Clock::now() - press.at > kPressWindow)
        {
            press.active = false;
            platform::Log(L"vending: {}'s press of {} opened no shop within {} s", who,
                          players::unreal::NameText(press.point),
                          std::chrono::duration_cast<std::chrono::seconds>(kPressWindow).count());
        }
        if (shopkeeper.active && !players::generation::Active(seat))
        {
            shopkeeper.active = false;
            platform::Log(L"vending: {} left with their press of shopkeeper {} pending", who,
                          players::unreal::NameText(shopkeeper.point));
        }
        else if (shopkeeper.active && !held && Clock::now() - shopkeeper.at > kShopkeeperWindow)
        {
            shopkeeper.active = false;
            platform::Log(L"vending: {}'s press of shopkeeper {} opened no shop within {} s", who,
                          players::unreal::NameText(shopkeeper.point),
                          std::chrono::duration_cast<std::chrono::seconds>(kShopkeeperWindow).count());
        }
        if (g_shop == seat && !players::generation::Active(seat))
        {
            g_shop = kNoShop;  // the seat's player left with their shop up: the one pad again
            input::pad_buttons::SetMenuPlayer(0);
            platform::Log(L"vending: {} left while their shop was up; pad 1 drives the menus again", who);
        }
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
    const bool presses = g_partyManager != 0 && Entry(image, g_end2Key, kEnd2Key, &End2Key);
    const bool cameras = platform::RunningBuild() == platform::BuildIndex::V1005
                             ? CameraCalls(image)
                             : Entry(image, g_camera, kCamera, &Camera);
    const bool closes  = Entry(image, g_cameraOff, kCameraOff, &CameraOff);
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

void Remove() noexcept
{
    g_vendingPreset = nullptr;  // Admits says no: the preset's name is not read again,
    g_presetName    = 0;        // and the one already read is forgotten
    bool restored   = g_cameraOff.Restore();
    for (auto& call : g_cameraCalls)
    {
        restored = call.Restore() && restored;
    }
    for (platform::EntryRedirect* entry : {&g_camera, &g_end2Key})
    {
        restored = entry->Restore() && restored;
    }
    if (!restored)
    {
        platform::Log(L"vending: a call or entry was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::world::vending
