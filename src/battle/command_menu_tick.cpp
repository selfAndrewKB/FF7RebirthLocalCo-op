#include "battle/command_menu_tick.h"

#include "battle/command_menu_tick_shared.h"
#include "battle/command_rules.h"
#include "battle/focus_scope.h"
#include "battle/menu_input_route.h"
#include "battle/menu_rules.h"
#include "battle/menu_tint.h"
#include "battle/seat_command_menus.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "platform/verified_code.h"
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
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

// The tick itself (battle/command_menu_tick.h): the mirrors, player 2's pass, the shields (the world slow,
// the
// command snapshot, the key guide), the trace watch, the placement and removal. Pad 2's presses and releases are
// battle/menu_input_route, the tint and the wait sound battle/menu_tint; this unit finds, places and
// removes them with its own sites.

namespace lss::battle::command_menu_tick {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// Facts read from the database (RE-p2-command-menu-tick.md §13.1; this build). Each call site's five
// bytes lie inside one aligned 16-byte block (checked again by CallRedirect).
constexpr std::uint8_t kTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                       0x24, 0x20, 0x57, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0xE8, 0x82, 0x1C, 0x67, 0x01};
constexpr platform::FunctionFact kTick{L"FieldCommandCtrl__Tick", 0x140A3D294, kTickBytes};
constexpr std::uintptr_t         kTickCall = 0x140C92978;  // the game mode's phase-0 global tick; the only caller
constexpr std::uint8_t kSlotBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0xB5, 0x38, 0x01};
constexpr platform::FunctionFact kSlot{L"FieldSys_SetFloatSlot3C0", 0x140D23A08, kSlotBytes};
constexpr std::uintptr_t         kSlotCall = 0x140D239EA;  // in FieldCommandCtrl_Tick_MenuSlowGameSpeed0
constexpr std::uint8_t kSnapshotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                           0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kSnapshot{L"EndUIRequestQueue_SetTypeCommandSnapshot", 0x141406348, kSnapshotBytes};
constexpr std::uintptr_t         kSnapshotCall = 0x140A3D613;
constexpr std::uint8_t kGuideShowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x20};
constexpr platform::FunctionFact kGuideShow{L"KeyGuide53_ShowMode", 0x1410E5AD8, kGuideShowBytes};
constexpr std::uintptr_t         kGuideShowCall = 0x140A64E05;
constexpr std::uint8_t kGuideMode8Bytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xDE, 0x10, 0x86, 0x00};
constexpr platform::FunctionFact kGuideMode8{L"KeyGuide53_SetMode8", 0x14184DE48, kGuideMode8Bytes};
constexpr std::uintptr_t         kGuideMode8Call = 0x140A64E1B;
constexpr std::uint8_t kHudModeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                          0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x3B, 0x6E, 0x00, 0x01};
constexpr platform::FunctionFact kSetHudMode{L"FieldCommandCtrl_SetBattleHudMode540", 0x1410A80E0, kHudModeBytes};
// The party list's input flag (ctrl+0x4A0 = party list +0x140): while 0 its show timer collapses the field
// menu every tick. The game sets it through EndUI23_ResumeHudInput(1) / SuspendHudInput(1), which act on
// its own controller only (RE-p2-command-menu-lifecycle.md §1).
constexpr std::uint8_t kResumeHudInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                                 0x89, 0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                 0xE6, 0x82, 0x42, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9};
constexpr platform::FunctionFact kResumeHudInput{L"FieldCommandCtrl_ResumeHudInput", 0x140C86C30, kResumeHudInputBytes};
// EndUI23_SuspendHudInput(u8 kind, u8 reset): acts on the UI system's controller slot.
constexpr std::uint8_t kSuspendHudInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
                                                  0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x57, 0x81, 0x42,
                                                  0x01, 0x48, 0x2B, 0xE0, 0x40, 0x8A, 0xF2, 0x0F, 0xB6, 0xD9};
constexpr platform::FunctionFact kSuspendHudInput{L"EndUI23_SuspendHudInput", 0x140C86DC4, kSuspendHudInputBytes};
constexpr std::uint8_t kPartyListInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48,
                                                 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x6E, 0x7F};
constexpr platform::FunctionFact kSetPartyListInput{L"PartyList360_SetInputEnabled140", 0x140C86FA0, kPartyListInputBytes};
constexpr std::uint32_t kPartyList      = 0x360;  // the controller's party list object
constexpr std::uint32_t kPartyListInput = 0x4A0;  // u8
using facts::kCmdSetupPending;
// FieldCommandCtrl_SetMenuFocusLayer783(ctrl, on): the menu's bottom focus layer (priority -1000),
// pushed and focused, or popped (RE-p2-command-menu-input.md §2).
constexpr std::uint8_t kSetMenuLayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18,
                                               0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x73, 0x80, 0x42,
                                               0x01, 0x48, 0x2B, 0xE0, 0x8A, 0x81, 0x83, 0x07, 0x00, 0x00};
constexpr platform::FunctionFact kSetMenuLayer{L"FieldCommandCtrl_SetMenuFocusLayer783", 0x140C86EA8, kSetMenuLayerBytes};
// UWidget::Visibility (ESlateVisibility, u8; logged). The command menu is shown and hidden by the game's
// own calls, mirrored on player 2's controller: ResumeHudInput(ctrl, 0) makes it Visible (once "setup
// done"), SuspendHudInput(0, _) Collapsed (RE-p2-command-menu-levers.md §7).
constexpr std::uint32_t kVisibility = 0xC3;
using facts::kCmdMenuWidget;
using facts::kCmdFieldMenu;

// The UI system's slots: the input object [2], the command controller [23]. Two values player 2's tick
// also writes need nothing here: EndUISystem[50]+0x20/+0x2C have no reader in the game, and the input
// object's +0x64 is per-player state kept in player 2's focus scope (RE-p2-command-menu-levers.md §5).
using facts::kUICommandController;
// The party manager's leader type (u8; PM+0x148 is the leader's weak pointer, scopes/leader_scope).
using facts::kPartyManagerLeaderType;
using facts::kPartyManagerPrimaryController;
using facts::kPartyManagerLeader;

// The controller (RE-p2-command-menu-tick.md §1, §2, §7).
using facts::kCmdStoredLeaderType;
using facts::kCmdMembers;
using facts::kCmdBattleInput;
using facts::kCmdSwitchingLayer;
using facts::kCmdRequests;
using facts::kCmdHudMode;
using facts::kCmdBlocked;
using facts::kCmdReady;
using facts::kCmdWaitFilter;
constexpr std::uint32_t kPinch            = 0x650;  // u8
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

// Set once at install.
platform::CallRedirect g_tickCall, g_slotCall, g_snapshotCall, g_guideShowCall, g_guideMode8Call;

/// The calls' targets as Install found them, for the redirects (placed at once, or on 1.005 from the game thread).
struct Targets
{
    std::uintptr_t slot = 0, snapshot = 0, show = 0, mode8 = 0, tick = 0;
};
Targets                            g_targets;
platform::DeferredSites g_placement;  // 1.005: the calls placed from the game thread (platform/deferred_sites.h)
constexpr const wchar_t* kUnavailable = L"UNAVAILABLE (a check failed): player 2 has no attack, guard or dodge";

bool PlaceSites(const platform::GameImage& image) noexcept;
HudModeFn      g_hudMode   = nullptr;
ResumeHudInputFn  g_resumeHudInput  = nullptr;
SuspendHudInputFn g_suspendHudInput = nullptr;
PartyListInputFn g_partyListInput = nullptr;
LeaderFn         g_resolveLeader  = nullptr;
std::uintptr_t g_endUISystem  = 0;
std::uintptr_t g_partyManager = 0;  // the address holding the party manager
SetMenuLayerFn g_setMenuLayer = nullptr;

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
static_assert(players::kMaxPlayers == 3, "g_seatSlots' initialiser names every player");

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

void SetRequest(std::uintptr_t controller, std::int32_t index, std::int32_t value) noexcept
{
    const auto data = ReadAt<std::uintptr_t>(controller + kCmdRequests);
    if (data != 0 && index < ReadAt<std::int32_t>(controller + kCmdRequests + 8))
    {
        WriteAt(data + 4 * static_cast<std::uint32_t>(index), value);
    }
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

/// Before player 2's tick. Pad 2's actions reach player 2's controller through the game's own input
/// handler (pad 2's presses and releases dispatched in player 2's UI): the press sets the request and
/// the release clears it, as for player 1; a suspend clears them through the tick's own
/// ClearOneShotRequests (the mirrored SuspendHudInput turns +0x25C off first). Here only the
/// leader-switch requests are cleared: a written exception (player 2's d-pad never switches the party
/// leader; the game's only switch path, BattleLeader_CycleSwitch, changes player 1's leader and no
/// per-player switch exists; RE-p2-command-menu-levers.md §15).
void WriteRequests(std::uintptr_t own) noexcept
{
    SetRequest(own, kPrevMember, 0);
    SetRequest(own, kNextMember, 0);
}

/// Battle input live (+0x25C = the member selecter's input flag, ctrl+0xF0+0x16C): the game turns it on and
/// off with EndUI23_ResumeHudInput(0) / SuspendHudInput(0, 0), on its own controller only; the same calls are
/// made for a seat's (the suspend inside the seat's UI, where the UI system's controller slot names the seat's).
/// Without it the input handler drops Attack, Unique and Guard, and the tick clears every request
/// (RE-p2-command-menu-actions.md; RE-p2-command-menu-lifecycle.md §3.3).
void MirrorBattleInput(int seat, std::uintptr_t own, std::uintptr_t one) noexcept
{
    const auto want = ReadAt<std::uint8_t>(one + kCmdBattleInput);
    const auto have = ReadAt<std::uint8_t>(own + kCmdBattleInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(own), std::uint8_t{0})
                               : platform::CallGuarded(g_suspendHudInput, std::uint8_t{0}, std::uint8_t{0});
    Line(std::format(L"{}'s battle input {} -> {} ({}){}: now {}", players::SeatName(seat), have, want,
                     want != 0 ? L"ResumeHudInput(0)" : L"SuspendHudInput(0, 0)", set ? L"" : L" FAULTED",
                     ReadAt<std::uint8_t>(own + kCmdBattleInput)));
}

/// A seat's controller follows the game's battle HUD mode through its own setter (§4.2, §7).
void MirrorHudMode(int seat, std::uintptr_t own, std::uintptr_t one, std::uint8_t memberType) noexcept
{
    const auto mode = ReadAt<std::uint8_t>(one + kCmdHudMode);
    if (ReadAt<std::uint8_t>(own + kCmdHudMode) != mode)
    {
        if (mode != 0)
        {
            // A written exception (RE-p2-command-menu-levers.md §4.2, §15): on its first battle frame
            // player 2's controller still holds player 1's field leader type, and the tick's leader-change
            // handler would clear the battle system's slot for player 1's type (BattleSys920_ClearTypeSlot
            // ForType) on a character player 1 is playing. No game path gives it player 2's type earlier.
            WriteAt(own + kCmdStoredLeaderType, memberType);
        }
        // The game's battle start and end call only this setter (RE-p2-command-menu-lifecycle.md §4.1); a
        // full reset would also clear "setup done" (+0x777), which only Setup puts back.
        const bool set = platform::CallGuarded(g_hudMode, reinterpret_cast<void*>(own), mode);
        Line(std::format(L"{}'s controller battle HUD mode -> {}{}", players::SeatName(seat), mode,
                         set ? L"" : L" (the setter FAULTED)"));
    }
    MirrorBattleInput(seat, own, one);
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

/// A seat's party list input flag follows player 1's, through the game's own calls (§10.2 R1).
void MirrorPartyListInput(int seat, std::uintptr_t own, std::uintptr_t one) noexcept
{
    const auto want = ReadAt<std::uint8_t>(one + kPartyListInput);
    const auto have = ReadAt<std::uint8_t>(own + kPartyListInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(own), std::uint8_t{1})
                               : platform::CallGuarded(g_partyListInput, reinterpret_cast<void*>(own + kPartyList),
                                                       std::uint8_t{0}, std::uint8_t{1});
    Line(std::format(L"{}'s party list input {} -> {} ({}){}", players::SeatName(seat), have, want,
                     want != 0 ? L"ResumeHudInput(1)" : L"SetInputEnabled(0)", set ? L"" : L" FAULTED"));
}

/// A seat's menu keeps the same bottom focus layer as player 1's (a battle event or help menu pops
/// player 1's: the seat's follows), through the controller's own setter. The mirrored Resume/Suspend
/// (MirrorBattleInput) already keep it in step, so this acts only on a change they did not carry.
void MirrorMenuLayer(int seat, std::uintptr_t own, std::uintptr_t one) noexcept
{
    const auto want = ReadAt<std::uint8_t>(one + kCmdMenuLayer);
    if (want == ReadAt<std::uint8_t>(own + kCmdMenuLayer))
    {
        return;
    }
    const bool set = platform::CallGuarded(g_setMenuLayer, reinterpret_cast<void*>(own), want);
    Line(std::format(L"{}'s menu focus layer -> {}{}", players::SeatName(seat), want, set ? L"" : L" (FAULTED)"));
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

    /// The parts the P1/P2 line shows.
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

void Watch(int seat, std::uintptr_t own, std::uintptr_t one) noexcept
{
    const auto index    = static_cast<std::size_t>(seat);
    auto&      watchLog = g_watchLogs[index];
    if (!platform::Enabled(platform::Level::Trace))
    {
        watchLog.Reset();  // off: the next line starts fresh
        return;
    }
    Snapshot now;
    constexpr std::array<std::uint32_t, 8> kCompared{kCmdSetupDone, kCmdBlocked,     kCmdBattleInput, kCmdMenuLayer,
                                                     kCmdSwitchingLayer, kCmdReady, kPartyListInput, kCmdSetupPending};
    for (std::size_t side = 0; side < 2; ++side)
    {
        const auto ctrl = side == 0 ? one : own;
        for (std::size_t i = 0; i < kCompared.size(); ++i)
        {
            now.compared[side][i] = ReadAt<std::uint8_t>(ctrl + kCompared[i]);
        }
        now.openPending[side] = ReadAt<std::int32_t>(ctrl + kCmdOpenPending);
    }
    now.path                = focus_scope::LivePathCount();
    now.menuVisibility      = {VisibilityOf(one, kCmdMenuWidget), VisibilityOf(own, kCmdMenuWidget)};
    now.fieldMenuVisibility = VisibilityOf(own, kCmdFieldMenu);
    void* leader = nullptr;
    now.leaderIsMember =
        platform::CallGuarded(g_resolveLeader, leader) && leader != nullptr && leader == players::generation::MemberOf(seat);
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        now.requests[i] = RequestOf(own, kLoggedRequests[i]);
    }
    constexpr std::array<std::uint32_t, 7> kBytes{kCmdHudMode,     kCmdBlocked,        kCmdReady,    kCmdReadyB,
                                                  kCmdBattleInput, kCmdSwitchingLayer, kHudSoundMode};
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = ReadAt<std::uint8_t>(own + kBytes[i]);
    }
    for (std::size_t i = 0; i < kLoggedGates.size(); ++i)
    {
        now.gates[i] = ReadAt<std::uint8_t>(own + kLoggedGates[i]);
    }
    now.guardLatch = ReadAt<std::int32_t>(own + kCmdGuardLatch);
    now.waitFilter = ReadAt<std::int32_t>(own + kCmdWaitFilter);
    now.storedType = ReadAt<std::uint8_t>(own + kCmdStoredLeaderType);
    now.pinch      = ReadAt<std::uint8_t>(own + kPinch);
    now.members    = {ReadAt<std::int32_t>(own + kCmdMembers), ReadAt<std::int32_t>(own + kCmdMembers + 4)};
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

/// A seat's controller's tick, inside the seat's scopes (§13.2).
void SeatPass(int seat, float deltaTime) noexcept
{
    const auto own    = reinterpret_cast<std::uintptr_t>(seat_command_menus::Controller(seat));
    const auto one    = ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController);
    void*      member = players::generation::MemberOf(seat);
    if (own == 0 || one == 0 || one == own || member == nullptr ||
        (ReadAt<std::uint8_t>(one + kCmdHudMode) == 0 && ReadAt<std::uint8_t>(own + kCmdHudMode) == 0))
    {
        menu_tint::Release(seat, L"released: the menu is not ticked (out of battle)");
        return;  // out of battle: the seat's controller has nothing to do yet
    }
    seat_ladder::Args args;
    args.seat          = seat;
    args.controller    = reinterpret_cast<void*>(own);
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
        MirrorHudMode(seat, own, one, args.memberType);
        MirrorPartyListInput(seat, own, one);
        MirrorMenuLayer(seat, own, one);
        WriteRequests(own);
        // A written exception (RE-p2-command-menu-levers.md §4.1, §15): the battle-HUD sound is one global
        // cue per edge, played by the first controller whose +0x774 differs from its HUD mode; a seat's
        // is pre-seeded with the value its own tick writes one line after the sound, so the cue plays once.
        WriteAt(own + kHudSoundMode, ReadAt<std::uint8_t>(own + kCmdHudMode));
        const bool ticked = platform::CallGuarded(reinterpret_cast<TickFn>(g_tickCall.Original()),
                                                  reinterpret_cast<void*>(own), deltaTime);
        SetPass(0, 0);
        g_passed = ticked;
        if (menu_tint::Holds(seat) && ReadAt<std::int32_t>(own + kCmdWaitFilter) == 0)
        {
            menu_tint::Release(seat, L"WARNING: the menu is idle but held a count; released");
        }
        if (!ticked)
        {
            scopes.Get<seat_ladder::FocusPart>()->scope.Abandon();
            menu_tint::Release(seat, L"released: the tick faulted");
            g_installed = false;
            Line(std::format(L"FAILED: {}'s controller's tick faulted; the further players' command controllers stop, "
                             L"and with them their attack, guard and dodge",
                             players::SeatName(seat)));
        }
        Watch(seat, own, one);
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

/// Replaces the world-slow write of every controller tick: any menu slows the world (policy A).
void Slot(std::uint8_t slot, float value) noexcept
{
    const auto original = reinterpret_cast<SlotFn>(g_slotCall.Original());
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

/// Replaces the per-type command snapshot write: player 2's pass leaves the game's in place.
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
    if (g_pass == 2 && seat_command_menus::KeyGuide(g_passSeat) == nullptr)
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
    if (g_pass == 2 && seat_command_menus::KeyGuide(g_passSeat) == nullptr)
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
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto tick = find(kTick), slot = find(kSlot), snapshot = find(kSnapshot), show = find(kGuideShow),
               mode8 = find(kGuideMode8), hud = find(kSetHudMode), resume = find(kResumeHudInput),
               suspend = find(kSuspendHudInput), listInput = find(kSetPartyListInput),
               leader = find(facts::kResolvePartyLeader), layer = find(kSetMenuLayer);
    const auto system  = image.Rebase(facts::kEndUISystem, facts::kUIFilterManager + sizeof(void*));
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
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
        g_endUISystem     = *system;
        g_partyManager    = *manager;
        g_setMenuLayer    = reinterpret_cast<SetMenuLayerFn>(*layer);
        g_targets         = {*slot, *snapshot, *show, *mode8, *tick};
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

/// The call sites, from what Install found (its checks passed). At install on the research build; on 1.005 from
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
    ok = ok && redirect(g_slotCall, kSlotCall, t.slot, &Slot) &&
         redirect(g_snapshotCall, kSnapshotCall, t.snapshot, &CommandSnapshot) &&
         redirect(g_guideShowCall, kGuideShowCall, t.show, &GuideShow) &&
         redirect(g_guideMode8Call, kGuideMode8Call, t.mode8, &GuideMode8) &&
         redirect(g_tickCall, kTickCall, t.tick, &Tick);
    g_installed = ok;
    if (!ok)
    {
        Remove();
    }
    else
    {
        menu_input_route::Install(image);  // pad 2's presses to player 2's menu in battle; its releases (optional)
    }
    platform::Log(L"command menu tick: {}", ok ? L"each further player's command controller ticks in battle and takes "
                                                 L"their attack, guard and dodge; their pads' presses reach their menus in "
                                                 L"battle (logged)"
                                               : kUnavailable);
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

void Remove() noexcept
{
    g_placement.Cancel();  // first: a placement still queued does nothing, and none lands between the restores
    menu_input_route::Remove();
    menu_tint::Remove();
    g_installed = false;
    for (auto* call : {&g_tickCall, &g_guideMode8Call, &g_guideShowCall, &g_snapshotCall, &g_slotCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::command_menu_tick
