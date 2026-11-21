#include "battle/command_menu_tick.h"

#include "battle/action_locks.h"
#include "battle/command_menu.h"
#include "battle/command_menu_tick_shared.h"
#include "battle/command_rules.h"
#include "battle/focus_scope.h"
#include "battle/menu_input_route.h"
#include "battle/menu_rules.h"
#include "battle/menu_tint.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/deferred_sites.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/settled_log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

// The tick itself (battle/command_menu_tick.h): the mirrors, each seat's pass, the shields (the world slow,
// the command snapshot, the key guide), the trace watch, the placement and removal. A seat's presses and releases
// are battle/menu_input_route, the tint and the wait sound battle/menu_tint; this unit finds, places and
// removes them with its own sites.

namespace lss::battle::command_menu_tick {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// Facts read from the database (research/RE-p2-command-menu-tick.md; this build). Each call site's five
// bytes lie inside one aligned 16-byte block (checked again by CallRedirect).
constexpr std::uint8_t kTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41,
                                       0x54, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0xE8, 0x80, 0x82};
constexpr platform::FunctionFact kTick{L"FieldCommandCtrl__Tick", 0x1408938E4, kTickBytes};
constexpr std::uintptr_t         kTickCall = 0x140B3564D;  // the game mode's phase-0 global tick; the only caller
constexpr std::uint8_t kSlotBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xBC, 0x68, 0x8D, 0x01};
constexpr platform::FunctionFact kSlot{L"FieldSys_SetFloatSlot3C0", 0x1408952B8, kSlotBytes};
constexpr std::uintptr_t         kSlotCall = 0x14089529A;  // in FieldCommandCtrl_Tick_MenuSlowGameSpeed0
constexpr std::uint8_t kSnapshotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                           0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kSnapshot{L"EndUIRequestQueue_SetTypeCommandSnapshot", 0x14088B6B0, kSnapshotBytes};
constexpr std::uintptr_t         kSnapshotCall = 0x140893CB3;
constexpr std::uint8_t kGuideShowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x20};
constexpr platform::FunctionFact kGuideShow{L"KeyGuide53_ShowMode", 0x14113115C, kGuideShowBytes};
constexpr std::uintptr_t         kGuideShowCall = 0x140DCF999;
constexpr std::uint8_t kGuideMode8Bytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD2, 0xD8, 0x89, 0x00};
constexpr platform::FunctionFact kGuideMode8{L"KeyGuide53_SetMode8", 0x1418CE2A4, kGuideMode8Bytes};
constexpr std::uintptr_t         kGuideMode8Call = 0x140DCF9AF;
constexpr std::uint8_t kHudModeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                          0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x97, 0x21, 0x93, 0x01};
constexpr platform::FunctionFact kSetHudMode{L"FieldCommandCtrl_SetBattleHudMode540", 0x1408399D4, kHudModeBytes};
// Its entry (`mov [rsp+8], rbx`, inside one block in both builds) is redirected: the game's one caller,
// EndUI_SetBattleHudMode, sets it on the game's controller at a battle's start and end, a field exit and the menu API's
// in-battle switch (research/RE-p2-command-menu-lifecycle.md).
constexpr std::size_t kSetHudModeFirst = 5;
// Outside battle the controller's leader type is the party roster's type at 0 (the party order: player 1's character),
// read by FieldCommandCtrl_GetLeaderType_PM150 with one call of facts::kRosterPlayerType; when that answers "none" the
// function falls back to the party manager's leader type, the one a seat's leader-type scope answers
// (research/RE-p2-field-command-menu.md). The call's five bytes cross a 16-byte block: placed from the game thread.
constexpr std::uintptr_t kFieldLeaderTypeCall = 0x1408901CC;
// The controller's "menu allowed" setter (ctrl, on, keepPending): off closes its member menus and clears its records,
// then +0x5E1 = on. Two callers in the game, both on the game's controller (the UI system's call and the on/off
// table's lock); its entry, `mov [rsp+8], rbx`, lies inside one block.
constexpr std::uint8_t kMenuAllowedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                              0x89, 0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kMenuAllowed{L"FieldCommandCtrl_SetMenuAllowed5E1", 0x14083A848, kMenuAllowedBytes};
constexpr std::size_t            kMenuAllowedFirst = 5;
// The party list's input flag (ctrl+0x4A0 = party list +0x140): while 0 its show timer collapses the field
// menu every tick. The game sets it through EndUI23_ResumeHudInput(1) / SuspendHudInput(1), which act on
// its own controller only (research/RE-p2-command-menu-lifecycle.md).
constexpr std::uint8_t kResumeHudInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                                 0x89, 0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                 0x66, 0x19, 0x93, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9};
constexpr platform::FunctionFact kResumeHudInput{L"FieldCommandCtrl_ResumeHudInput", 0x14083A200, kResumeHudInputBytes};
// EndUI23_SuspendHudInput(u8 kind, u8 reset): acts on the UI system's controller slot.
constexpr std::uint8_t kSuspendHudInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
                                                  0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x6B, 0x15, 0x93,
                                                  0x01, 0x48, 0x2B, 0xE0, 0x40, 0x8A, 0xF2, 0x0F, 0xB6, 0xD9};
constexpr platform::FunctionFact kSuspendHudInput{L"EndUI23_SuspendHudInput", 0x14083A600, kSuspendHudInputBytes};
constexpr std::uint8_t kPartyListInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48,
                                                 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x4E, 0x18};
constexpr platform::FunctionFact kSetPartyListInput{L"PartyList360_SetInputEnabled140", 0x14083A310, kPartyListInputBytes};
constexpr std::uint32_t kPartyList      = 0x360;  // the controller's party list object
constexpr std::uint32_t kPartyListInput = 0x4A0;  // u8
using facts::kCmdSetupPending;
// FieldCommandCtrl_SetMenuFocusLayer783(ctrl, on): the menu's bottom focus layer (priority -1000),
// pushed and focused, or popped (research/RE-p2-command-menu-input.md).
constexpr std::uint8_t kSetMenuLayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18,
                                               0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x87, 0x14, 0x93,
                                               0x01, 0x48, 0x2B, 0xE0, 0x8A, 0x81, 0x83, 0x07, 0x00, 0x00};
constexpr platform::FunctionFact kSetMenuLayer{L"FieldCommandCtrl_SetMenuFocusLayer783", 0x14083A6E4, kSetMenuLayerBytes};
// UWidget::Visibility (ESlateVisibility, u8; logged). The command menu is shown and hidden by the game's
// own calls, mirrored on each seat's controller: ResumeHudInput(ctrl, 0) makes it Visible (once "setup
// done"), SuspendHudInput(0, _) Collapsed (research/RE-p2-command-menu-levers.md).
constexpr std::uint32_t kVisibility = 0xC3;
using facts::kCmdMenuWidget;
using facts::kCmdFieldMenu;

// The UI system's slots: the input object [2], the command controller [23]. Two values a seat's tick
// also writes need nothing here: EndUISystem[50]+0x20/+0x2C have no reader in the game, and the input
// object's +0x64 is per-player state kept in the seat's focus scope (research/RE-p2-command-menu-levers.md).
using facts::kUICommandController;
// The party manager's leader type (u8; PM+0x148 is the leader's weak pointer, scopes/leader_scope).
using facts::kPartyManagerLeaderType;
using facts::kPartyManagerPrimaryController;
using facts::kPartyManagerLeader;

// The controller (research/RE-p2-command-menu-tick.md).
using facts::kCmdStoredLeaderType;
using facts::kCmdMembers;
using facts::kCmdBattleInput;
using facts::kCmdSwitchingLayer;
using facts::kCmdHudMode;
using facts::kCmdBlocked;
using facts::kCmdMenuAllowed;
using facts::kCmdWaitFilter;
using facts::kCmdGuardLatch;
constexpr std::uint32_t kHudSoundMode     = 0x774;  // u8
using facts::kCmdReadyB;
using facts::kCmdMenuLayer;
constexpr std::array<std::uint32_t, 4> kLoggedGates{0x776, 0x77C, 0x77F, 0x787};  // u8, logged only
// Also the shortcut commands [14..18] (the Shortcut*_Second keys), the synergy requests [20..23], and what
// keeps the shortcut modifier [6] through a release: [7], [41], [42] (RE-p2-shortcut-modifier.md).
constexpr std::array<std::int32_t, 23> kLoggedRequests{0,  1,  5,  6,  7,  12, 13, 14, 15, 16, 17, 18,
                                                       20, 21, 22, 23, 25, 32, 37, 38, 39, 41, 42};
constexpr std::size_t kSnapshotTypes = 15;
using facts::kCmdOpenPending;
using facts::kCmdSetupDone;

using TickFn      = void (*)(void* controller, float deltaTime);
using SlotFn      = void (*)(std::uint8_t slot, float value);
using SnapshotFn  = void (*)(void* queue, const std::uint64_t* commands, const std::uint8_t* categories);
using GuideShowFn = std::uint8_t (*)(void* guide, std::uint8_t mode);
using GuideMode8Fn = void (*)();
using HudModeFn   = void (*)(void* controller, std::uint8_t mode);
using ResumeHudInputFn = void (*)(void* controller, std::uint8_t kind);
using SuspendHudInputFn = void (*)(std::uint8_t kind, std::uint8_t reset);
using PartyListInputFn = void (*)(void* partyList, std::uint8_t on, std::uint8_t applyVisibility);
using LeaderFn         = void* (*)();
using SetMenuLayerFn = void (*)(void* controller, std::uint8_t on);
using MemberTypeFn   = std::uint8_t (*)(std::int32_t index);
using MenuAllowedFn  = void (*)(void* controller, std::uint8_t on, std::uint8_t keepPending);

// Set once at install.
platform::CallRedirect  g_tickCall, g_slotCall, g_snapshotCall, g_guideShowCall, g_guideMode8Call, g_fieldLeaderCall;
platform::EntryRedirect g_hudModeEntry;      // the battle HUD mode's setter, repeated on each seat's controller
platform::EntryRedirect g_menuAllowedEntry;  // the menu-allowed setter, the same

/// The calls' targets as Install found them, for the redirects (placed at once, or on 1.005 from the game thread).
struct Targets
{
    std::uintptr_t slot = 0, snapshot = 0, show = 0, mode8 = 0, tick = 0, memberType = 0;
};
Targets                            g_targets;
platform::DeferredSites g_placement;  // 1.005: the calls placed from the game thread (platform/deferred_sites.h)
constexpr const wchar_t* kUnavailable = L"UNAVAILABLE (a check failed): players 2 and 3 have no attack, guard or dodge";

bool PlaceSites(const platform::GameImage& image) noexcept;
void LogMenuSlow(const wchar_t* why) noexcept;  // the slow-down's state, at Info (below)
HudModeFn      g_hudMode   = nullptr;
ResumeHudInputFn  g_resumeHudInput  = nullptr;
SuspendHudInputFn g_suspendHudInput = nullptr;
PartyListInputFn g_partyListInput = nullptr;
LeaderFn         g_resolveLeader  = nullptr;
std::uintptr_t g_endUISystem  = 0;
std::uintptr_t g_partyManager = 0;  // the address holding the party manager
SetMenuLayerFn g_setMenuLayer = nullptr;
MenuAllowedFn  g_menuAllowed  = nullptr;

// The slow-down switch (the menu-slow setting): read by every world-slow write and by the tint's gates, set by the
// settings file at install and by the page or the console afterwards.
std::atomic<bool> g_slows{true};

// Game thread.
bool         g_installed = false;
int          g_pass      = 0;  // 1: the game's controller ticks; 2: a seat's; 3: a seat's pad in its UI
int          g_passSeat  = 0;  // the seat of a pass 2 or 3 (0 otherwise)
std::uint64_t g_frame     = 0;  // ticks of the game's controller
std::uint64_t g_oneFrame  = 0;  // the frame player 1's slot value is from
float        g_oneSlot   = 1.0F;
std::array<float, players::kMaxPlayers>         g_seatSlots{1.0F, 1.0F, 1.0F};  // each seat's slot value; index 0 unused
std::array<std::uint64_t, players::kMaxPlayers> g_seatFrames{};  // the frame each seat's slot value is from
float        g_written   = 1.0F;
bool         g_passed    = false;  // the last seat's controller ticked this frame
unsigned     g_refusals  = 0, g_guideSkips = 0;
std::array<unsigned, players::kMaxPlayers> g_snapshotEntries{};  // each seat's last logged count; index 0 unused
std::uint64_t g_passes     = 0;  // the seats' passes run
// The battle HUD mode each seat's controller last refused (not set up yet), so the refusal is logged once; -1 none.
std::array<int, players::kMaxPlayers> g_hudModeRefused{-1, -1, -1};
// The menu-allowed value each seat's controller last did not take at a catch-up, so it is logged once and tried again
// only every kMenuAllowedRetryGap ticks; -1 none. A seat whose setter faulted is no longer set at all.
std::array<int, players::kMaxPlayers>  g_menuAllowedRefused{-1, -1, -1};
std::array<bool, players::kMaxPlayers> g_menuAllowedFaulted{};
constexpr std::uint64_t                kMenuAllowedRetryGap = 60;
// The command menu's own calls on a seat's controller in flight outside a pass (OwnMemberScope): while any is, the
// leader-type read outside battle answers the seat's member, as in a pass.
int g_ownMember = 0;
static_assert(players::kMaxPlayers == 3, "g_seatSlots' initialiser names every player");
// Outside battle (each seat's; index 0 unused): the seats' menus open only with both of their calls in place; the
// field's first pass and the leader-type answer are logged once per stretch outside battle, then counted.
bool                                            g_fieldMenus = false;
std::array<bool, players::kMaxPlayers>          g_fieldTicked{};
std::array<bool, players::kMaxPlayers>          g_fieldMenuOpen{};  // its menu open at its last pass outside battle
std::array<bool, players::kMaxPlayers>          g_fieldLeaderSaid{};
std::array<std::uint64_t, players::kMaxPlayers> g_fieldLeaderAnswers{};
int                                             g_menuAllowedSaid = -1;  // the game's last logged value; -1 none

void WatchLine(const std::wstring& text) noexcept
{
    platform::LogTrace(L"command menu tick: {}", text);
}

/// An event line of `level`.
void Line(const std::wstring& text, platform::Level level = platform::Level::Info) noexcept
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"command menu tick: {}", text);
    }
}

/// A battle is on for a seat's controller `ctrl` beside the game's `ctrl1`: either one's battle HUD mode is set (the
/// seat's catches up with the game's at its pass).
bool InBattle(std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    return (ctrl1 != 0 && ReadAt<std::uint8_t>(ctrl1 + kCmdHudMode) != 0) ||
           (ctrl != 0 && ReadAt<std::uint8_t>(ctrl + kCmdHudMode) != 0);
}

}  // namespace

int Pass() noexcept
{
    return g_pass;
}

int PassSeat() noexcept
{
    return g_passSeat;
}

void SetPass(int pass, int seat) noexcept
{
    g_pass     = pass;
    g_passSeat = pass >= 2 ? seat : 0;
}

bool Installed() noexcept
{
    return g_installed;
}

std::uintptr_t UiSystem() noexcept
{
    return g_endUISystem;
}

std::uintptr_t PartyManager() noexcept
{
    return g_partyManager;
}

namespace {

/// Battle input live (+0x25C = the member selecter's input flag, ctrl+0xF0+0x16C): the game turns it on and
/// off with EndUI23_ResumeHudInput(0) / SuspendHudInput(0, 0), on its own controller only; the same calls are
/// made for a seat's (the suspend inside the seat's UI, where the UI system's controller slot names the seat's).
/// Without it the input handler drops Attack, Unique and Guard, and the tick clears every request
/// (research/RE-p2-command-menu-actions.md, RE-p2-command-menu-lifecycle.md).
void CopyBattleInput(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    const auto want = ReadAt<std::uint8_t>(ctrl1 + kCmdBattleInput);
    const auto have = ReadAt<std::uint8_t>(ctrl + kCmdBattleInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(ctrl), std::uint8_t{0})
                               : platform::CallGuarded(g_suspendHudInput, std::uint8_t{0}, std::uint8_t{0});
    Line(std::format(L"{}'s battle input {} -> {} ({}, copied from the game's){}: now {}", players::SeatName(seat), have,
                     want, want != 0 ? L"ResumeHudInput(0)" : L"SuspendHudInput(0, 0)", set ? L"" : L" FAULTED",
                     ReadAt<std::uint8_t>(ctrl + kCmdBattleInput)));
}

/// The battle HUD mode's setter as the game's own (the redirect's trampoline once placed).
HudModeFn HudModeSetter() noexcept
{
    return g_hudModeEntry.IsInstalled() ? reinterpret_cast<HudModeFn>(g_hudModeEntry.Original()) : g_hudMode;
}

/// The game's battle HUD mode set on a seat's controller through the game's setter (the setter's other
/// effects ride along: its menus and members reset, its requests cleared at a start).
bool SetSeatHudMode(int seat, std::uintptr_t ctrl, std::uint8_t mode, std::uint8_t memberType,
                    const wchar_t* why) noexcept
{
    if (mode != 0)
    {
        // A written exception (research/RE-p2-command-menu-levers.md): on its first battle frame the seat's
        // controller still holds player 1's field leader type, and the tick's leader-change handler would clear the
        // battle system's slot for player 1's type (BattleSys920_ClearTypeSlotForType) on a character player 1 is
        // playing. No game path gives it the seat's type earlier.
        WriteAt(ctrl + kCmdStoredLeaderType, memberType);
    }
    const bool set   = platform::CallGuarded(HudModeSetter(), reinterpret_cast<void*>(ctrl), mode);
    const bool taken = set && ReadAt<std::uint8_t>(ctrl + kCmdHudMode) == mode;
    auto&      tried = g_hudModeRefused[static_cast<std::size_t>(seat)];
    if (taken || !set)
    {
        tried = -1;
        Line(std::format(L"{}'s controller battle HUD mode -> {} ({}){}", players::SeatName(seat), mode, why,
                         set ? L"" : L"; the setter FAULTED"));
    }
    else if (tried != mode)
    {
        // The setter does nothing on a controller not yet set up: tried again each pass, logged once.
        tried = mode;
        Line(std::format(L"{}'s controller did not take battle HUD mode {} ({}): not set up yet; tried again at its "
                         L"passes",
                         players::SeatName(seat), mode, why),
             platform::Level::Debug);
    }
    return taken;
}

/// The mode the game set while the seat's controller could not take it (its player out, no member): caught up in the
/// seat's pass, inside its scopes.
void CatchUpHudMode(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1, std::uint8_t memberType) noexcept
{
    const auto mode = ReadAt<std::uint8_t>(ctrl1 + kCmdHudMode);
    if (ReadAt<std::uint8_t>(ctrl + kCmdHudMode) != mode)
    {
        SetSeatHudMode(seat, ctrl, mode, memberType, L"caught up");
    }
    CopyBattleInput(seat, ctrl, ctrl1);
}

/// A setter's call on `controller` may be the game's own on its controller: installed, on the game thread, not inside
/// a seat's pass or press or the command menu's own call on a seat's controller, and not on a seat's controller at all
/// (inside a seat's UI context, as around its character's tick, the UI system's controller slot names the seat's, and
/// a call the game makes there is the seat's own).
bool GameCallHere(const void* controller) noexcept
{
    return g_installed && g_pass < 2 && g_ownMember == 0 && controller != nullptr &&
           players::generation::OnGameThread() && command_menu::SeatOfController(controller) < 1;
}

/// After a setter's call on the game's controller: `repeat(seat, ctrl, memberType)` on each seat's whose player plays
/// a member, inside that seat's focus and leader scopes (the setters pop focus layers and read the party leader's
/// type). A seat it cannot reach now is caught up at its next pass (its wait logged at Debug); a seat `wanted(ctrl)` says
/// needs nothing is skipped before its scopes. `what` and `value` name the call in the log (the setters are hot: the
/// text is made only for a line).
template <class Wanted, class Repeat>
void RepeatOnSeats(void* controller, const wchar_t* what, int value, Wanted&& wanted, Repeat&& repeat) noexcept
{
    // No repeat inside a seat's pass or press or the menu's own call on a seat (the pass's catch-up covers the seats).
    if (!GameCallHere(controller))
    {
        return;
    }
    const auto ctrl1 = ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController);
    if (reinterpret_cast<std::uintptr_t>(controller) != ctrl1)
    {
        return;
    }
    const auto manager = ReadAt<std::uintptr_t>(g_partyManager);
    const auto party   = [manager] {
        return std::array<std::uint64_t, 3>{ReadAt<std::uint64_t>(manager + kPartyManagerPrimaryController),
                                            ReadAt<std::uint64_t>(manager + kPartyManagerLeader),
                                            ReadAt<std::uint8_t>(manager + kPartyManagerLeaderType)};
    };
    const auto before = manager != 0 ? party() : std::array<std::uint64_t, 3>{};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto ctrl   = reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
        void*      member = players::generation::MemberOf(seat);
        if (!players::generation::Active(seat) || ctrl == 0 || ctrl == ctrl1 || member == nullptr || !wanted(ctrl))
        {
            continue;
        }
        seat_ladder::Args args;
        args.seat       = seat;
        args.controller = reinterpret_cast<void*>(ctrl);
        if (!seat_ladder::FillMember(args, member))
        {
            continue;
        }
        // The setter's focus-layer pops and leader-type read only (no Rules part: args.leaderType unused).
        seat_ladder::SeatScopes scopes(seat_ladder::Focus | seat_ladder::Leader | seat_ladder::LeaderType, 0u, args);
        if (!scopes.Held())
        {
            if (platform::Enabled(platform::Level::Debug))
            {
                Line(std::format(L"{}'s {} {} waits for its pass: {}", players::SeatName(seat), what, value,
                                 scopes.Refusal()),
                     platform::Level::Debug);
            }
            continue;
        }
        const OwnMemberScope own;  // the setter's leader-type read answers the seat's member, as in its pass
        repeat(seat, ctrl, args.memberType);
    }
    if (manager != 0 && party() != before)
    {
        Line(std::format(L"WARNING: the primary controller or the party leader differs after the {} {}'s repeats", what,
                         value));
    }
}

/// Replaces the battle HUD mode's setter: the game's call, then the same on each seat's controller.
void SetHudMode(void* controller, std::uint8_t mode) noexcept
{
    HudModeSetter()(controller, mode);
    RepeatOnSeats(controller, L"battle HUD mode", mode, [](std::uintptr_t) { return true; },
                  [mode](int seat, std::uintptr_t ctrl, std::uint8_t memberType) {
                      SetSeatHudMode(seat, ctrl, mode, memberType, L"the game's call");
                  });
}

/// The menu-allowed setter as the game's own (the redirect's trampoline once placed).
MenuAllowedFn MenuAllowedSetter() noexcept
{
    return g_menuAllowedEntry.IsInstalled() ? reinterpret_cast<MenuAllowedFn>(g_menuAllowedEntry.Original())
                                            : g_menuAllowed;
}

/// The game's menu-allowed value set on a seat's controller through the game's setter (off closes the seat's menus).
/// After a fault the seat's controller is no longer set: its menu keeps the value it had.
void SetSeatMenuAllowed(int seat, std::uintptr_t ctrl, std::uint8_t on, std::uint8_t keepPending,
                        const wchar_t* why) noexcept
{
    const auto slot = static_cast<std::size_t>(seat);
    if (g_menuAllowedFaulted[slot])
    {
        return;
    }
    const bool set   = platform::CallGuarded(MenuAllowedSetter(), reinterpret_cast<void*>(ctrl), on, keepPending);
    const bool taken = set && ReadAt<std::uint8_t>(ctrl + kCmdMenuAllowed) == on;
    auto&      tried = g_menuAllowedRefused[slot];
    if (!set)
    {
        g_menuAllowedFaulted[slot] = true;
        Line(std::format(L"FAILED: the menu-allowed setter faulted on {}'s controller ({} -> {}); its menu no longer "
                         L"follows where the game allows or forbids player 1's",
                         players::SeatName(seat), why, on));
    }
    else if (taken)
    {
        tried = -1;
        Line(std::format(L"{}'s menu allowed -> {} ({})", players::SeatName(seat), on, why), platform::Level::Debug);
    }
    else if (tried != on)
    {
        tried = on;
        Line(std::format(L"{}'s controller did not take menu allowed {} ({}); tried again every {} ticks at its passes",
                         players::SeatName(seat), on, why, kMenuAllowedRetryGap));
    }
}

/// Replaces the menu-allowed setter: the game's call, then the same on each seat's controller, so a seat's menu is
/// forbidden and allowed again where player 1's is.
void SetMenuAllowed(void* controller, std::uint8_t on, std::uint8_t keepPending) noexcept
{
    MenuAllowedSetter()(controller, on, keepPending);
    if (!GameCallHere(controller))
    {
        return;
    }
    if (reinterpret_cast<std::uintptr_t>(controller) == ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController) &&
        g_menuAllowedSaid != on)
    {
        g_menuAllowedSaid = on;
        Line(std::format(L"menu allowed {} on the game's controller; players 2 and 3's controllers follow", on));
    }
    // An allow on a seat's controller that already allows its menu changes nothing (an off still closes what is open),
    // nor one while the seat's own action holds its menu (its pass allows it once that action ends).
    RepeatOnSeats(controller, L"menu allowed", on,
                  [on](std::uintptr_t ctrl) { return on == 0 || ReadAt<std::uint8_t>(ctrl + kCmdMenuAllowed) != on; },
                  [on, keepPending](int seat, std::uintptr_t ctrl, std::uint8_t) {
                      if (on == 0 || action_locks::Held(seat) == 0)
                      {
                          SetSeatMenuAllowed(seat, ctrl, on, keepPending, L"the game's call");
                      }
                  });
}

/// The seat whose member's player type is `type` (a playing seat), or 0.
int SeatPlayingType(std::uint8_t type) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        const auto memberType = member != nullptr ? session::party_member::PlayerTypeOf(member) : std::nullopt;
        if (memberType && *memberType == type)
        {
            return seat;
        }
    }
    return 0;
}

/// Replaces the leader type's read of battle member 0 outside battle: in the command menu's own calls on a seat's
/// controller (a seat's pass or press, or an OwnMemberScope), inside the seat's leader-type scope (the party manager's
/// leader type names a playing seat's member, which it never does outside one), it answers "none", so the game's own
/// fallback gives the seat's member; otherwise the game's answer, other modules' leader-type scopes included. A
/// written exception (the game's fallback, chosen by the mod; research/RE-p2-field-command-menu.md): no game path gives
/// a seat's controller its own member outside battle.
std::uint8_t FieldLeaderType(std::int32_t index) noexcept
{
    const std::uint8_t first = reinterpret_cast<MemberTypeFn>(g_fieldLeaderCall.Original())(index);
    if (!players::generation::OnGameThread() || (g_pass < 2 && g_ownMember == 0))
    {
        return first;
    }
    const auto manager = ReadAt<std::uintptr_t>(g_partyManager);
    const auto held    = manager != 0 ? ReadAt<std::uint8_t>(manager + kPartyManagerLeaderType) : first;
    const int  seat    = held != first ? SeatPlayingType(held) : 0;
    if (seat < 1)
    {
        return first;
    }
    const auto slot = static_cast<std::size_t>(seat);
    ++g_fieldLeaderAnswers[slot];
    if (!g_fieldLeaderSaid[slot])
    {
        g_fieldLeaderSaid[slot] = true;
        Line(std::format(L"{}'s controller's leader type outside battle: its member (type {}) instead of battle member 0 "
                         L"(type {})",
                         players::SeatName(seat), held, first));
    }
    return facts::kNoPlayerType;
}

/// A controller's widget (its FWeakObjectPtr at `offset`), or null.
void* WidgetOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    return players::weak_object::ObjectOf(ReadAt<std::uint64_t>(controller + offset));
}

/// A widget's visibility (0 Visible, 1 Collapsed, ...), or 0xFF without the widget.
std::uint8_t VisibilityOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    const auto widget = reinterpret_cast<std::uintptr_t>(WidgetOf(controller, offset));
    return widget != 0 ? ReadAt<std::uint8_t>(widget + kVisibility) : 0xFF;
}

/// A seat's party list input flag follows player 1's, through the game's own calls.
void CopyPartyListInput(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    const auto want = ReadAt<std::uint8_t>(ctrl1 + kPartyListInput);
    const auto have = ReadAt<std::uint8_t>(ctrl + kPartyListInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(ctrl), std::uint8_t{1})
                               : platform::CallGuarded(g_partyListInput, reinterpret_cast<void*>(ctrl + kPartyList),
                                                       std::uint8_t{0}, std::uint8_t{1});
    Line(std::format(L"{}'s party list input {} -> {} ({}, copied from the game's){}", players::SeatName(seat), have,
                     want, want != 0 ? L"ResumeHudInput(1)" : L"SetInputEnabled(0)", set ? L"" : L" FAULTED"));
}

/// A seat's menu keeps the same bottom focus layer as player 1's (a battle event or help menu pops
/// player 1's: the seat's follows), through the controller's own setter. The copied Resume/Suspend
/// (CopyBattleInput) already keep it in step, so this acts only on a change they did not carry.
void CopyMenuLayer(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    const auto want = ReadAt<std::uint8_t>(ctrl1 + kCmdMenuLayer);
    if (want == ReadAt<std::uint8_t>(ctrl + kCmdMenuLayer))
    {
        return;
    }
    const bool set = platform::CallGuarded(g_setMenuLayer, reinterpret_cast<void*>(ctrl), want);
    Line(std::format(L"{}'s menu focus layer -> {} (copied from the game's){}", players::SeatName(seat), want,
                     set ? L"" : L" (FAULTED)"));
}

/// What the log shows of a seat's controller, as raw values.
struct Snapshot
{
    std::array<std::int32_t, kLoggedRequests.size()> requests{};
    std::array<std::uint8_t, 7>  bytes{};  // 540 541 5E1 782 25C 258 774
    std::array<std::uint8_t, kLoggedGates.size()> gates{};
    std::int32_t guardLatch = 0, waitFilter = 0;
    std::uint8_t storedType = 0, pinch = 0;
    std::array<std::int32_t, 2> members{};
    // Beside player 1's: 777, 541, 25C, 783, 258, 5E1 (u8) and the pending open (i32), each [0] player 1, [1] the seat.
    std::array<std::array<std::uint8_t, 8>, 2> compared{};
    // The command menus' visibility, [0] P1 [1] the seat's (0 Visible, 1 Collapsed).
    std::array<std::uint8_t, 2>                menuVisibility{0xFF, 0xFF};
    std::uint8_t                               fieldMenuVisibility = 0xFF;  // the seat's
    bool                                       leaderIsMember = false;       // inside the pass
    std::array<std::int32_t, 2>                openPending{};
    std::int32_t                               path = 0;  // the seat's live focus path length (inside the pass)
    bool operator==(const Snapshot&) const = default;

    /// The parts the comparison line shows (player 1's values beside the seat's).
    bool SameSidesAs(const Snapshot& other) const noexcept
    {
        return compared == other.compared && openPending == other.openPending && menuVisibility == other.menuVisibility &&
               path == other.path && fieldMenuVisibility == other.fieldMenuVisibility && leaderIsMember == other.leaderIsMember;
    }

    /// The parts the seat's controller line shows.
    bool SameControllerAs(const Snapshot& other) const noexcept
    {
        return requests == other.requests && bytes == other.bytes && gates == other.gates && guardLatch == other.guardLatch &&
               waitFilter == other.waitFilter && storedType == other.storedType && pinch == other.pinch &&
               members == other.members;
    }
};
// The action requests flip with each press: they are logged once they hold (platform/settled_log.h).
constexpr std::uint32_t kWatchStillFrames = 30;
using Requests = std::array<std::int32_t, kLoggedRequests.size()>;
// By seat; index 0 unused.
std::array<platform::SettledLog<Snapshot, Requests>, players::kMaxPlayers> g_watchLogs{
    platform::SettledLog<Snapshot, Requests>(kWatchStillFrames),
    platform::SettledLog<Snapshot, Requests>(kWatchStillFrames),
    platform::SettledLog<Snapshot, Requests>(kWatchStillFrames)};
std::array<Snapshot, players::kMaxPlayers> g_logged{};
std::array<bool, players::kMaxPlayers>     g_loggedOnce{};

void Watch(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    const auto index    = static_cast<std::size_t>(seat);
    auto&      watchLog = g_watchLogs[index];
    if (!platform::Enabled(platform::Level::Trace))
    {
        watchLog.Reset();  // off: the next line starts fresh
        return;
    }
    Snapshot now;
    constexpr std::array<std::uint32_t, 8> kCompared{kCmdSetupDone,      kCmdBlocked,     kCmdBattleInput, kCmdMenuLayer,
                                                     kCmdSwitchingLayer, kCmdMenuAllowed, kPartyListInput, kCmdSetupPending};
    for (std::size_t side = 0; side < 2; ++side)
    {
        const auto sideCtrl = side == 0 ? ctrl1 : ctrl;
        for (std::size_t i = 0; i < kCompared.size(); ++i)
        {
            now.compared[side][i] = ReadAt<std::uint8_t>(sideCtrl + kCompared[i]);
        }
        now.openPending[side] = ReadAt<std::int32_t>(sideCtrl + kCmdOpenPending);
    }
    now.path                = focus_scope::LivePathCount();
    now.menuVisibility      = {VisibilityOf(ctrl1, kCmdMenuWidget), VisibilityOf(ctrl, kCmdMenuWidget)};
    now.fieldMenuVisibility = VisibilityOf(ctrl, kCmdFieldMenu);
    void* leader = nullptr;
    now.leaderIsMember =
        platform::CallGuarded(g_resolveLeader, leader) && leader != nullptr && leader == players::generation::MemberOf(seat);
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        now.requests[i] = RequestOf(ctrl, kLoggedRequests[i]);
    }
    constexpr std::array<std::uint32_t, 7> kBytes{kCmdHudMode,     kCmdBlocked,        kCmdMenuAllowed, kCmdReadyB,
                                                  kCmdBattleInput, kCmdSwitchingLayer, kHudSoundMode};
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = ReadAt<std::uint8_t>(ctrl + kBytes[i]);
    }
    for (std::size_t i = 0; i < kLoggedGates.size(); ++i)
    {
        now.gates[i] = ReadAt<std::uint8_t>(ctrl + kLoggedGates[i]);
    }
    now.guardLatch = ReadAt<std::int32_t>(ctrl + kCmdGuardLatch);
    now.waitFilter = ReadAt<std::int32_t>(ctrl + kCmdWaitFilter);
    now.storedType = ReadAt<std::uint8_t>(ctrl + kCmdStoredLeaderType);
    now.pinch      = ReadAt<std::uint8_t>(ctrl + facts::kCmdDanger);
    now.members    = {ReadAt<std::int32_t>(ctrl + kCmdMembers), ReadAt<std::int32_t>(ctrl + kCmdMembers + 4)};
    Snapshot steady = now;
    steady.requests = {};
    if (!watchLog.Offer(steady, now.requests))
    {
        return;
    }
    const bool sidesChanged      = !g_loggedOnce[index] || !now.SameSidesAs(g_logged[index]);
    const bool controllerChanged = !g_loggedOnce[index] || !now.SameControllerAs(g_logged[index]);
    g_logged[index]     = now;
    g_loggedOnce[index] = true;
    try
    {
        std::wstring requests;
        for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
        {
            requests += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
        }
        const auto& b = now.bytes;
        const auto& g = now.gates;
        const auto  side = [&now](std::size_t i) { return std::format(L"{}/{}", now.compared[0][i], now.compared[1][i]); };
        if (sidesChanged)
        {
            WatchLine(std::format(L"P1/P{}: 777 {} 541 {} 25C {} 783 {} 258 {} 5E1 {} 4A0 {} 775 {} open {}/{} command "
                                  L"menu visibility {}/{} | P{} path {}, field menu visibility {}, leader is P{}'s "
                                  L"member {}",
                                  seat + 1, side(0), side(1), side(2), side(3), side(4), side(5), side(6), side(7),
                                  now.openPending[0], now.openPending[1], now.menuVisibility[0], now.menuVisibility[1],
                                  seat + 1, now.path, now.fieldMenuVisibility, seat + 1, now.leaderIsMember ? 1 : 0));
        }
        if (controllerChanged)
        {
            WatchLine(std::format(L"{}'s controller: req{} | guard latch {} | 540={} 541={} 5E1={} 782={} 25C={} "
                                  L"258={} 774={} | type {} leader {} ally {} | wait {} pinch {} | 776={} 77C={} 77F={} "
                                  L"787={} | slot 0: P1 {:.3f} P{} {:.3f} written {:.3f}",
                                  players::SeatName(seat), requests, now.guardLatch, b[0], b[1], b[2], b[3], b[4], b[5],
                                  b[6], now.storedType, now.members[0], now.members[1], now.waitFilter, now.pinch, g[0],
                                  g[1], g[2], g[3], g_oneSlot, seat + 1, g_seatSlots[index], g_written));
        }
    }
    catch (...)
    {
    }
}

void Refused(int seat, const wchar_t* why) noexcept
{
    ++g_refusals;
    if (g_refusals <= 5 || g_refusals % 600 == 0)
    {
        Line(std::format(L"{}'s pass skipped: {} ({} time(s))", players::SeatName(seat), why, g_refusals));
    }
}

/// The menu-allowed value the game stored on its controller without its setter (the on/off table's lock cleared) or set
/// while the seat's controller could not take it, made on the seat's through the setter in the seat's pass (a value it
/// did not take is tried again every kMenuAllowedRetryGap ticks), shut while the seat's own actions hold a lock on its
/// menu (battle/action_locks), as the table's applier shuts player 1's. An off clears the seat's records and pending
/// open (keepPending 0): a catch-up leaves nothing open that the game has forbidden.
void CatchUpMenuAllowed(int seat, std::uintptr_t ctrl, std::uintptr_t ctrl1) noexcept
{
    const bool ownLock = action_locks::Held(seat) > 0;
    const auto want    = ownLock ? std::uint8_t{0} : ReadAt<std::uint8_t>(ctrl1 + kCmdMenuAllowed);
    if (g_menuAllowed == nullptr || ReadAt<std::uint8_t>(ctrl + kCmdMenuAllowed) == want)
    {
        return;
    }
    if (g_menuAllowedRefused[static_cast<std::size_t>(seat)] == want && g_frame % kMenuAllowedRetryGap != 0)
    {
        return;
    }
    SetSeatMenuAllowed(seat, ctrl, want, 0, ownLock ? L"their own action" : L"caught up");
}

/// After a seat's pass outside battle: its first is logged once per stretch outside battle, with the menu member it
/// resolved (expect the seat's member's type). A leader-type change in the pass means the game's field leader change
/// ran on the seat's controller: to its member's type when its player's member changed, a WARNING to any other.
void NoteFieldPass(int seat, std::uintptr_t ctrl, std::uint8_t typeBefore, std::uint8_t memberType) noexcept
{
    const auto slot = static_cast<std::size_t>(seat);
    const auto type = ReadAt<std::uint8_t>(ctrl + kCmdStoredLeaderType);
    if (type != typeBefore)
    {
        Line(std::format(L"{}{}'s controller's leader type changed in its pass outside battle ({} -> {}; its member's "
                         L"{}): the game's leader change ran on it",
                         type == memberType ? L"" : L"WARNING: ", players::SeatName(seat), typeBefore, type,
                         memberType));
    }
    // Its member list's focus layer is pushed while the menu is open (the open's last step).
    const bool open = ReadAt<std::uint8_t>(ctrl + kCmdSwitchingLayer) != 0;
    if (open != g_fieldMenuOpen[slot])
    {
        g_fieldMenuOpen[slot] = open;
        Line(std::format(L"{}'s command menu {} outside battle (menu member {} type {}; open request {}, open pending {})",
                         players::SeatName(seat), open ? L"opened" : L"closed", ReadAt<std::int32_t>(ctrl + kCmdMembers),
                         type, RequestOf(ctrl, kReqOpenMenu), ReadAt<std::int32_t>(ctrl + kCmdOpenPending)));
    }
    if (!g_fieldTicked[slot])
    {
        g_fieldTicked[slot] = true;
        Line(std::format(L"{}'s command menu ticks outside battle (menu member {} type {}, its member's type {}; menu "
                         L"allowed {}; {} leader-type answer(s) so far)",
                         players::SeatName(seat), ReadAt<std::int32_t>(ctrl + kCmdMembers), type, memberType,
                         ReadAt<std::uint8_t>(ctrl + kCmdMenuAllowed), g_fieldLeaderAnswers[slot]));
    }
}

/// A seat's controller's tick, inside the seat's scopes.
void SeatPass(int seat, float deltaTime) noexcept
{
    const auto ctrl   = reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
    const auto ctrl1  = ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController);
    void*      member = players::generation::MemberOf(seat);
    const auto slot   = static_cast<std::size_t>(seat);
    const bool battle = InBattle(ctrl, ctrl1);
    if (battle)
    {
        g_fieldTicked[slot]     = false;  // the next stretch outside battle logs its first pass again
        g_fieldLeaderSaid[slot] = false;
        g_fieldMenuOpen[slot]   = false;
    }
    if (ctrl == 0 || ctrl1 == 0 || ctrl1 == ctrl || member == nullptr || !SeatTicks(seat))
    {
        menu_tint::Release(seat, battle || g_fieldMenus
                                     ? L"released: the menu is not ticked (not set up yet)"
                                     : L"released: the menu is not ticked (out of battle, where it does not open)");
        return;
    }
    seat_ladder::Args args;
    args.seat          = seat;
    args.controller    = reinterpret_cast<void*>(ctrl);
    const auto manager = ReadAt<std::uintptr_t>(g_partyManager);
    if (!seat_ladder::FillMember(args, member) || manager == 0)
    {
        return Refused(seat, L"the member could not be read");
    }
    const auto before = std::array<std::uint64_t, 3>{ReadAt<std::uint64_t>(manager + kPartyManagerPrimaryController),
                                                     ReadAt<std::uint64_t>(manager + kPartyManagerLeader),
                                                     ReadAt<std::uint8_t>(manager + kPartyManagerLeaderType)};
    args.leaderType   = static_cast<std::uint8_t>(before[2]);
    {
        // Every scope of the seat's, the rules last; the target fields only in battle (optional).
        seat_ladder::SeatScopes scopes(seat_ladder::Focus | seat_ladder::Sticks | seat_ladder::Primary |
                                           seat_ladder::Buttons | seat_ladder::Targets | seat_ladder::Leader |
                                           seat_ladder::LeaderType | seat_ladder::Rules,
                                       seat_ladder::Targets, args);
        if (!scopes.Held())
        {
            return Refused(seat, scopes.Refusal());
        }
        SetPass(2, seat);
        ++g_passes;
        CatchUpHudMode(seat, ctrl, ctrl1, args.memberType);
        CatchUpMenuAllowed(seat, ctrl, ctrl1);
        CopyPartyListInput(seat, ctrl, ctrl1);
        CopyMenuLayer(seat, ctrl, ctrl1);
        auto typeBefore = ReadAt<std::uint8_t>(ctrl + kCmdStoredLeaderType);
        if (!battle && typeBefore != args.memberType && typeBefore == args.leaderType)
        {
            // A written exception, the battle one's twin (SetSeatHudMode): a controller set up while the leader-type
            // answer could not be given (its member not yet known, or before the answer was placed) holds player 1's
            // type, and the tick's field leader change would clear the battle system's slot for a character player 1
            // is playing. Its member's type is what the game's Setup writes under the answer.
            WriteAt(ctrl + kCmdStoredLeaderType, args.memberType);
            Line(std::format(L"{}'s controller held player 1's leader type (type {}) outside battle: its member's (type "
                             L"{}) written, as its Setup writes",
                             players::SeatName(seat), typeBefore, args.memberType));
            typeBefore = args.memberType;
        }
        // The seat's requests are the game's own handler's (the pad's presses and releases dispatched in the seat's UI
        // set and clear them, as for player 1), the leader-switch requests included: the tick's switch step runs the
        // game's cycle for the seat inside this pass (session/party_switch.h).
        // A written exception (research/RE-p2-command-menu-levers.md): the battle-HUD sound is one global
        // cue per edge, played by the first controller whose +0x774 differs from its HUD mode; a seat's
        // is pre-seeded with the value its own tick writes one line after the sound, so the cue plays once.
        WriteAt(ctrl + kHudSoundMode, ReadAt<std::uint8_t>(ctrl + kCmdHudMode));
        const bool ticked = platform::CallGuarded(reinterpret_cast<TickFn>(g_tickCall.Original()),
                                                  reinterpret_cast<void*>(ctrl), deltaTime);
        SetPass(0, 0);
        g_passed = ticked;
        if (ticked && !battle)
        {
            NoteFieldPass(seat, ctrl, typeBefore, args.memberType);
        }
        if (menu_tint::Holds(seat) && ReadAt<std::int32_t>(ctrl + kCmdWaitFilter) == 0)
        {
            menu_tint::Release(seat, L"WARNING: the menu is idle but held a count; released");
        }
        if (!ticked)
        {
            scopes.Get<seat_ladder::FocusPart>()->scope.Abandon();
            menu_tint::Release(seat, L"released: the tick faulted");
            g_installed = false;
            Line(std::format(L"FAILED: {}'s controller's tick faulted; players 2 and 3's command controllers stop, "
                             L"and with them their attack, guard and dodge",
                             players::SeatName(seat)));
        }
        Watch(seat, ctrl, ctrl1);
    }
    const auto after = std::array<std::uint64_t, 3>{ReadAt<std::uint64_t>(manager + kPartyManagerPrimaryController),
                                                    ReadAt<std::uint64_t>(manager + kPartyManagerLeader),
                                                    ReadAt<std::uint8_t>(manager + kPartyManagerLeaderType)};
    if (after != before)
    {
        Line(std::format(L"WARNING: the primary controller or the party leader differs after {}'s pass",
                         players::SeatName(seat)));
    }
}

/// Replaces the game's controller tick call: the game's controller, then each seat's in seat order.
void Tick(void* controller, float deltaTime) noexcept
{
    menu_tint::FollowSwitch();  // outside every pass: a switch since the last tick reaches any waiting menu first
    ++g_frame;
    SetPass(1, 0);
    reinterpret_cast<TickFn>(g_tickCall.Original())(controller, deltaTime);
    SetPass(0, 0);
    g_passed = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_installed && players::generation::OnGameThread() && players::generation::Active(seat))
        {
            SeatPass(seat, deltaTime);
        }
        else
        {
            menu_tint::Release(seat, L"released: the menu is not ticked (its player is out)");
        }
    }
}

/// Replaces the world-slow write of every controller tick: any menu slows the world (policy A); with the slow-down
/// switched off every menu asks for full speed.
void Slot(std::uint8_t slot, float value) noexcept
{
    const auto original = reinterpret_cast<SlotFn>(g_slotCall.Original());
    value               = menu_rules::SlowAsked(g_slows.load(std::memory_order_relaxed), value);
    if (g_pass == 2 && players::SeatIndex(g_passSeat))
    {
        const int seat                                 = g_passSeat;
        g_seatSlots[static_cast<std::size_t>(seat)]    = value;
        g_seatFrames[static_cast<std::size_t>(seat)]   = g_frame;
        if (g_oneFrame != g_frame)
        {
            Line(std::format(L"WARNING: {}'s slow value came without player 1's this frame; written as is",
                             players::SeatName(seat)));
        }
        // The menus that ticked before this one: player 1's, then the earlier seats'.
        menu_rules::SlowValue earlier[players::kMaxPlayers] = {{g_oneFrame == g_frame, g_oneSlot}};
        for (int before = 1; before < seat; ++before)
        {
            earlier[before] = {g_seatFrames[static_cast<std::size_t>(before)] == g_frame,
                               g_seatSlots[static_cast<std::size_t>(before)]};
        }
        g_written = menu_rules::SlowWrittenAmong(earlier, seat, value);
        return original(slot, g_written);
    }
    if (g_pass == 1)
    {
        g_oneSlot  = value;
        g_oneFrame = g_frame;
        g_written  = value;
    }
    original(slot, value);
}

/// Replaces the per-type command snapshot write: a seat's pass leaves the game's in place.
void CommandSnapshot(void* queue, const std::uint64_t* commands, const std::uint8_t* categories) noexcept
{
    if (g_pass != 2)
    {
        return reinterpret_cast<SnapshotFn>(g_snapshotCall.Original())(queue, commands, categories);
    }
    std::array<std::uint64_t, kSnapshotTypes> entries{};
    platform::CopyGuarded(entries.data(), commands, sizeof(entries));
    const auto used = static_cast<unsigned>(std::count_if(entries.begin(), entries.end(), [](auto e) { return e != 0; }));
    auto& logged = g_snapshotEntries[players::SeatIndex(g_passSeat) ? static_cast<std::size_t>(g_passSeat) : 0];
    if (used != logged)
    {
        logged = used;
        Line(std::format(L"{}'s command snapshot (not written): {} type(s) with a command",
                         players::SeatName(g_passSeat), used),
             platform::Level::Debug);
    }
}

std::uint8_t GuideShow(void* guide, std::uint8_t mode) noexcept
{
    // With the seat's own key guide in the slot (battle/focus_scope), the seat's controller drives it
    // natively; without one, the seat's pass leaves the shared guide alone.
    if (g_pass == 2 && command_menu::KeyGuide(g_passSeat) == nullptr)
    {
        ++g_guideSkips;
        if (platform::Enabled(platform::Level::Debug))
        {
            Line(std::format(L"{}'s key-guide mode {} not shown ({} time(s))", players::SeatName(g_passSeat), mode,
                             g_guideSkips),
                 platform::Level::Debug);
        }
        return 0;
    }
    return reinterpret_cast<GuideShowFn>(g_guideShowCall.Original())(guide, mode);
}

void GuideMode8() noexcept
{
    if (g_pass == 2 && command_menu::KeyGuide(g_passSeat) == nullptr)
    {
        ++g_guideSkips;
        if (platform::Enabled(platform::Level::Debug))
        {
            Line(std::format(L"{}'s key-guide mode 8 not set ({} time(s))", players::SeatName(g_passSeat), g_guideSkips),
                 platform::Level::Debug);
        }
        return;
    }
    reinterpret_cast<GuideMode8Fn>(g_guideMode8Call.Original())();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    // The menu-slow setting (one of the table's words), set before any redirect is placed: no write reads it yet.
    g_slows.store(platform::settings::Of(platform::settings::kMenuSlow).word == L"on");
    try
    {
        LogMenuSlow(platform::settings::Why(platform::settings::kMenuSlow).c_str());
    }
    catch (...)
    {
        LogMenuSlow(L"the settings file");  // out of memory for the reason's text
    }
    try
    {
        control::RegisterOption(
            platform::settings::kMenuSlow,
            [](const std::wstring& word) {
                if (!g_installed)
                {
                    return control::Outcome{false, L"the command menu's calls are not in place; the switch does nothing"};
                }
                SetMenuSlow(word == L"on", L"the console");
                return control::Outcome{};
            },
            [] { return std::wstring(MenuSlow() ? L"on" : L"off"); });
    }
    catch (...)
    {
        platform::Log(L"command menu: the slow-down option could not be registered (out of memory?); the page cannot set it");
    }
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto tick = find(kTick), slot = find(kSlot), snapshot = find(kSnapshot), show = find(kGuideShow),
               mode8 = find(kGuideMode8), hud = find(kSetHudMode), resume = find(kResumeHudInput),
               suspend = find(kSuspendHudInput), listInput = find(kSetPartyListInput),
               leader = find(facts::kResolvePartyLeader), layer = find(kSetMenuLayer);
    const auto system  = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto manager = players::game_globals::Address(players::game_globals::Global::PartyManager);
    const bool tint    = menu_tint::Find(image);  // each unit's finds run whatever the tick's found (each logs its own)
    const bool route   = menu_input_route::Find(image);
    const bool ok      = tick && slot && snapshot && show && mode8 && hud && resume && suspend && listInput && leader &&
                    layer && players::weak_object::Known() && system && manager && tint && route;
    if (ok)
    {
        g_hudMode         = reinterpret_cast<HudModeFn>(*hud);
        g_resumeHudInput  = reinterpret_cast<ResumeHudInputFn>(*resume);
        g_suspendHudInput = reinterpret_cast<SuspendHudInputFn>(*suspend);
        g_partyListInput  = reinterpret_cast<PartyListInputFn>(*listInput);
        g_resolveLeader   = reinterpret_cast<LeaderFn>(*leader);
        g_endUISystem     = system;
        g_partyManager    = manager;
        g_setMenuLayer    = reinterpret_cast<SetMenuLayerFn>(*layer);
        g_targets         = {*slot, *snapshot, *show, *mode8, *tick};
        // Outside battle (optional: without them the seats' menus open in battle only).
        const auto memberType = find(facts::kRosterPlayerType);
        const auto allowed    = find(kMenuAllowed);
        g_targets.memberType  = memberType.value_or(0);
        g_menuAllowed         = allowed ? reinterpret_cast<MenuAllowedFn>(*allowed) : nullptr;
    }
    if (!ok)
    {
        g_installed = false;
        Remove();
        platform::Log(L"command menu tick: {}", kUnavailable);
        return false;
    }
    // The tick's call and the key guide's mode-8 call cross a 16-byte block in 1.005: every site is placed from
    // the game thread, between its ticks (platform/deferred_sites.h).
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"command menu tick: {}",
                      placed ? L"its calls are placed from the game thread (1.005: two cross a 16-byte block)"
                             : L"UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

namespace {

/// The call sites, from what Install found (its checks passed). At install on 1.004; otherwise from
/// the game thread.
bool PlaceSites(const platform::GameImage& image) noexcept
{
    const Targets& t  = g_targets;
    bool           ok = true;
    // The shields first, the tick last: no pass runs before its shields are in place.
    const auto redirect = [&image](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && g_placement.Place(call, *live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    ok = ok && menu_tint::Place(image, g_placement);
    const auto hudFirst = image.EntryInstruction(kSetHudMode.address, kSetHudMode.entryBytes.first(kSetHudModeFirst));
    ok = ok && g_hudModeEntry.Install(reinterpret_cast<std::uintptr_t>(g_hudMode), hudFirst,
                                      reinterpret_cast<std::uintptr_t>(&SetHudMode));
    ok = ok && redirect(g_slotCall, kSlotCall, t.slot, &Slot) &&
         redirect(g_snapshotCall, kSnapshotCall, t.snapshot, &CommandSnapshot) &&
         redirect(g_guideShowCall, kGuideShowCall, t.show, &GuideShow) &&
         redirect(g_guideMode8Call, kGuideMode8Call, t.mode8, &GuideMode8);
    // Outside battle, before the tick: a seat's first pass there already resolves its own member.
    if (ok && g_menuAllowed != nullptr && t.memberType != 0)
    {
        const auto allowedFirst =
            image.EntryInstruction(kMenuAllowed.address, kMenuAllowed.entryBytes.first(kMenuAllowedFirst));
        g_fieldMenus = g_menuAllowedEntry.Install(reinterpret_cast<std::uintptr_t>(g_menuAllowed), allowedFirst,
                                                  reinterpret_cast<std::uintptr_t>(&SetMenuAllowed)) &&
                       redirect(g_fieldLeaderCall, kFieldLeaderTypeCall, t.memberType, &FieldLeaderType);
        if (!g_fieldMenus && g_menuAllowedEntry.IsInstalled() && !g_menuAllowedEntry.Restore())
        {
            platform::Log(L"command menu tick: the menu-allowed setter was changed after us; left in place (it passes "
                          L"through)");
        }
    }
    ok = ok && redirect(g_tickCall, kTickCall, t.tick, &Tick);
    g_installed = ok;
    if (!ok)
    {
        Remove();
    }
    else
    {
        menu_input_route::Install(image);  // a seat's presses to its menu; its releases (optional)
    }
    platform::Log(L"command menu tick: {}", ok ? L"player 2's and 3's command controllers tick and take their attack, "
                                                 L"guard and dodge in battle; their pads' presses reach their menus "
                                                 L"(logged)"
                                               : kUnavailable);
    if (ok)
    {
        platform::Log(L"command menu tick: {}",
                      g_fieldMenus ? L"players 2 and 3's command menus open outside battle too, each with its own "
                                     L"member's commands, items and magic, where the game allows player 1's (logged)"
                                   : L"players 2 and 3's command menus open in battle only (a check outside battle "
                                     L"failed)");
    }
    return ok;
}

}  // namespace

bool RefocusMenu(void* controller) noexcept
{
    if (g_resumeHudInput == nullptr || g_setMenuLayer == nullptr || controller == nullptr)
    {
        return false;
    }
    const auto layer = ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(controller) + kCmdMenuLayer);
    if (layer != 0 && !platform::CallGuarded(g_setMenuLayer, controller, std::uint8_t{0}))
    {
        return false;
    }
    return platform::CallGuarded(g_resumeHudInput, controller, std::uint8_t{0});
}

bool BattleInputOn(const void* controller) noexcept
{
    return controller != nullptr &&
           ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(controller) + kCmdBattleInput) != 0;
}

bool SeatTicks(int seat) noexcept
{
    const auto ctrl  = reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
    const auto ctrl1 = g_endUISystem != 0 ? ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController) : 0;
    return ctrl != 0 && ctrl1 != 0 && command_menu::Ready(seat) && (g_fieldMenus || InBattle(ctrl, ctrl1));
}

OwnMemberScope::OwnMemberScope() noexcept
{
    ++g_ownMember;
}

OwnMemberScope::~OwnMemberScope()
{
    --g_ownMember;
}

bool MenuSlow() noexcept
{
    return g_slows.load(std::memory_order_relaxed);
}

void SetMenuSlow(bool slows, const wchar_t* why) noexcept
{
    if (g_slows.exchange(slows) != slows)
    {
        LogMenuSlow(why);  // a waiting menu's grey and muffle follow at the next controller tick (menu_tint)
    }
}

namespace {

void LogMenuSlow(const wchar_t* why) noexcept
{
    if (MenuSlow())
    {
        platform::Log(L"command menu: an open battle menu slows the world, greys the screen and muffles the sound, as the "
                      L"game does ({})",
                      why);
    }
    else
    {
        platform::Log(L"command menu: an open battle menu leaves the battle at full speed, with no grey and no muffled "
                      L"sound ({})",
                      why);
    }
}

}  // namespace

void Remove() noexcept
{
    g_placement.Cancel();  // first: a placement still queued does nothing, and none lands between the restores
    menu_input_route::Remove();
    menu_tint::Remove();
    g_installed          = false;
    g_fieldMenus         = false;
    g_menuAllowedFaulted = {};  // a later install starts every seat's follow and its lines afresh
    g_menuAllowedRefused = {-1, -1, -1};
    g_menuAllowedSaid    = -1;
    g_fieldTicked        = {};
    g_fieldMenuOpen      = {};
    g_fieldLeaderSaid    = {};
    g_fieldLeaderAnswers = {};
    for (auto* call : {&g_tickCall, &g_fieldLeaderCall, &g_guideMode8Call, &g_guideShowCall, &g_snapshotCall, &g_slotCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
    if (g_menuAllowedEntry.IsInstalled() && !g_menuAllowedEntry.Restore())
    {
        platform::Log(L"command menu tick: the menu-allowed setter was changed after us; left in place (it passes "
                      L"through)");
    }
    if (g_hudModeEntry.IsInstalled() && !g_hudModeEntry.Restore())
    {
        platform::Log(L"command menu tick: the battle HUD mode's setter was changed after us; left in place (it passes "
                      L"through)");
    }
}

}  // namespace lss::battle::command_menu_tick
