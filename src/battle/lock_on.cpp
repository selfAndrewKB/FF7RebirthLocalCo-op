#include "battle/lock_on.h"

#include "battle/command_menu.h"
#include "battle/focus_scope.h"
#include "battle/panel_gate_code.h"
#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/hud.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/build_value.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::battle::lock_on {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// Facts read from the database (RE-p2-targeting-scoped-tick.md §4.3, RE-p2-lock-on.md §R.4; this
// build). Each function's first bytes, through its stack probe, are checked before it is used.
constexpr std::uint8_t kTickTargetingBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0xAC, 0x74, 0x01};
constexpr std::uint8_t kSetLockBytes[]       = {0xB8, 0x98, 0x00, 0x00, 0x00, 0xE8, 0xCE, 0x3B, 0x81, 0x00,
                                                0x48, 0x2B, 0xE0, 0x8A, 0x81, 0x8C, 0x00, 0x00, 0x00};
constexpr std::uint8_t kMenuSelectBytes[]    = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0xF9, 0x41, 0x01};
constexpr std::uint8_t kMenuBusyBytes[]      = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xFE, 0xF9, 0x41, 0x01,
                                                0x48, 0x2B, 0xE0, 0xE8, 0x06, 0x74, 0xB7, 0xFF};
constexpr std::uint8_t kPanelClearedBytes[]  = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xA0,
                                                0x84, 0x6D, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kPanelSetBytes[]      = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0xA3, 0x6E,
                                                0xFE, 0x48, 0x2B, 0xE0, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x40};
constexpr std::uint8_t kMarkerNameBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xFC,
                                                0x06, 0x42, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kCursorReleaseBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xDC, 0x42, 0x07, 0x01};
constexpr std::uint8_t kDeactivateBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x38, 0xB3, 0xEA, 0x00};
constexpr std::uint8_t kTargetListBytes[]    = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xCE, 0xFE, 0x41, 0x01,
                                                0x48, 0x2B, 0xE0, 0xE8, 0xD6, 0x78, 0xB7, 0xFF};
constexpr std::uint8_t kModuleOnBytes[]      = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                                0x00, 0x00, 0x00, 0xE8, 0xA0, 0xB6, 0xEA, 0x00};

constexpr platform::FunctionFact kTickTargeting{L"BattleTarget_TickTargeting", 0x140964230, kTickTargetingBytes};
constexpr platform::FunctionFact kSetLock{L"BattleTarget_SetLockMode8C", 0x14189B358, kSetLockBytes};
constexpr platform::FunctionFact kMenuSelect{L"BattleTarget_IsMenuTargetSelectOpen", 0x140C8F560, kMenuSelectBytes};
constexpr platform::FunctionFact kMenuBusy{L"BattleTarget_IsMenuTargetSelectBusy", 0x140C8F528, kMenuBusyBytes};
constexpr platform::FunctionFact kPanelCleared{L"EndUI4_QueueTargetCleared", 0x1439D6A84, kPanelClearedBytes};
constexpr platform::FunctionFact kPanelSet{L"EndUI4_QueueTargetSet", 0x1439C4B30, kPanelSetBytes};
constexpr platform::FunctionFact kMarkerName{L"EndUI__MarkerList_SetTargetOwnerName", 0x140C8E828, kMarkerNameBytes};
constexpr platform::FunctionFact kCursorRelease{L"EndUI22_ReleaseTargetCursor", 0x14103AC48, kCursorReleaseBytes};
constexpr platform::FunctionFact kDeactivate{L"PrimaryPCM_Op2CA8_SlotCall18", 0x141203BEC, kDeactivateBytes};
// Whether the command controller in EndUISystem[23] has its target list open (entries 10/11/13).
constexpr platform::FunctionFact kTargetList{L"EndUI23_IsCommandTargetEntryActive", 0x140C8F058, kTargetListBytes};
// Its body, bool(command controller), which the wrapper above tail-jumps to with EndUISystem[23]. 1.005 inlined the
// wrapper into its only caller (RE-1005-player-two.md); the body is byte for byte the research build's
// (RE-1005-battle-2.md), so on 1.005 the mod calls it with the same slot the wrapper read.
constexpr std::uint8_t kTargetListImplBytes[] = {0x8B, 0x41, 0x34, 0x33, 0xD2, 0x39, 0x51, 0x38,
                                                 0x4C, 0x8B, 0xC9, 0x0F, 0x4D, 0x41, 0x38};
constexpr platform::FunctionFact kTargetListImpl{L"EndUI23_IsCommandTargetEntryActive_Impl", 0x140C8F07C,
                                                 kTargetListImplBytes};
constexpr platform::FunctionFact kModuleOn{L"PrimaryPCM_Op2CA8_SlotFlag44", 0x141203880, kModuleOnBytes};

// The targeting tick's two calls in BattleSystem_Tick (time stopped with the menu's target select;
// in battle), and the tick's three writes with one copy for both players: the two requests for
// player 1's target panel (each call takes a weak reference made for it and releases it at its end)
// and the marker list's target owner name.
constexpr std::uintptr_t kTickTargetingCalls[] = {0x140C91AA2, 0x140C91CC0};
constexpr std::uintptr_t kPanelClearedSite     = 0x14295F0A5;  // in the cold chunk of the "cleared" request (the call straddles a block)
constexpr std::uintptr_t kPanelClearedResume   = 0x14295F0AD;
constexpr std::uint8_t   kPanelClearedCallBytes[] = {0xE8, 0xC1, 0x79, 0x07, 0x01};
// the request's call, checked: E8 to EndUI4_QueueTargetCleared
constexpr platform::FunctionFact kPanelClearedCall{L"the cleared request's call of QueueTargetCleared", 0x14295F0BE,
                                                   kPanelClearedCallBytes};
constexpr std::uintptr_t kPanelClearedSkipTo   = 0x14295F0C3;  // after the request's call
constexpr std::uintptr_t kPanelSetCall         = 0x14295F139;  // crosses a block on 1.005 (placed from the game thread)
constexpr std::uintptr_t kMarkerNameCall       = 0x140C8F9C9;

// The battle system (a pointer global) and its target fields (§R.1, §9; S-TGT5 §5).
using facts::kBattleStateGlobal;
constexpr std::uint32_t  kCursor       = 0x68;  // the target cursor widget
constexpr std::uint32_t  kDisplayed    = 0x70;  // TWeakPtr {object, reference controller}
constexpr std::uint32_t  kTrace        = 0x80;  // the cursor's async trace handle
constexpr std::uint32_t  kLock         = 0x8C;
constexpr std::uint32_t  kLatch        = 0x8D;
constexpr std::uint32_t  kFreeTime     = 0x90;  // float: the lock camera's rest after a stick move
// The UI system's bar manager (EndUISystem[31]), whose +0x140 is the "targeted" key the tick sets from
// the displayed target (a TWeakPtr).
constexpr std::uint32_t  kTargetedKey  = 0x140;
// TRefCtrlTS counts; the LockOn module (layer 7) in a Battle operator's module table, its active
// byte and cached target (logged).
constexpr std::uint8_t   kLockOnLayer    = 7;
using facts::kCameraManagerBattleOperator;
constexpr std::uint32_t  kModuleActive   = 0x44;
constexpr std::uint32_t  kModuleTarget   = 0x3F8;


struct Pair  // a TWeakPtr as the game holds it
{
    std::uintptr_t object     = 0;
    std::uintptr_t controller = 0;
    bool operator==(const Pair&) const = default;
};

using Kind  = platform::ByteContext::Kind;
using Range = platform::ByteContext::Range;

using TickFn         = void (*)(void* system, float deltaTime);
using SetLockFn      = std::uint8_t (*)(void* system, std::uint8_t lock);
using TestFn         = std::uint8_t (*)();
using ListImplFn     = std::uint8_t (*)(void* commandController);
using ReleaseFn      = void (*)(std::uintptr_t controller);
using WeakReleaseFn  = void (*)(void* weakPtr);
using PanelSetFn     = void (*)(void* weakPtr, float x, float y);
using MarkerNameFn   = void (*)(void* owner);
using CursorFn       = void (*)(void* widget);
using LayerFn        = void (*)(std::uint8_t layer);
using LayerTestFn    = std::uint8_t (*)(std::uint8_t layer);

// Set once at install.
platform::CallRedirect g_tickTargetingCalls[2];
platform::CodeRedirect g_panelClearedGate;
platform::CallRedirect g_panelClearedCall;  // 1.005: the "cleared" request's call, which fits one block there
platform::CallRedirect g_panelSetCall;
platform::CallRedirect g_markerNameCall;
// 1.005: the call sites are placed from the game thread (both targeting calls and the "set" call cross a block),
// with the image kept for it (platform/deferred_sites.h).
platform::DeferredSites g_placement;
bool           g_installed   = false;  // the scope's facts: the TargetScope works
bool           g_runReady    = false;  // player 2's run and its skipped writes are installed
SetLockFn      g_setLock     = nullptr;
TestFn         g_menuSelect  = nullptr;
TestFn         g_menuBusy    = nullptr;
TestFn         g_targetList  = nullptr;  // optional: the log of player 2's target list
ListImplFn     g_targetListImpl = nullptr;  // the same on 1.005 (the wrapper's body; see kTargetListImpl)
ReleaseFn      g_release     = nullptr;
WeakReleaseFn  g_weakRelease = nullptr;
CursorFn       g_cursorRelease = nullptr;
LayerFn        g_deactivate  = nullptr;
LayerTestFn    g_moduleOn    = nullptr;
std::uintptr_t g_battleSystem = 0;  // the address holding the battle system
std::uintptr_t g_endUISystem  = 0;

// Game thread. A seat's copies of the battle system's fields (the displayed target owns one weak
// reference) and of the bar manager's targeted key (one weak reference), read back at the end of
// each of its scopes.
struct SeatTargets
{
    Pair           displayed;
    Pair           targetedKey;
    std::uintptr_t cursor   = 0;
    std::uint64_t  trace    = 0;
    std::uint32_t  freeTime = 0;
    std::uint8_t   lock     = 0;
    std::uint8_t   latch    = 0;
    bool           listOpen = false;  // the seat's command target list, as last seen in its run
    std::uint32_t  listOpenings = 0;
};
std::array<SeatTargets, players::kMaxPlayers> g_seats{};  // by seat; index 0 unused (player 1's are the game's own)

bool          g_held      = false;  // a TargetScope holds (one at a time, whichever seat's)
bool          g_run       = false;  // inside a seat's targeting run: the shared writes are skipped
PanelGate     g_panelGate;          // the same, for the "cleared" request (battle/panel_gate_code.h)
bool          g_enabled   = true;
bool          g_failed    = false;
std::uint64_t g_runs = 0, g_refusals = 0, g_menuPauses = 0, g_panelSkips = 0, g_lockRestores = 0;

/// `seat`'s fields. Every caller has checked the seat; a bad one (a bug) gets a scratch record, cleared each time, and
/// one warning, so no seat's fields are touched (and no weak reference is released twice).
SeatTargets& SeatOf(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        return g_seats[static_cast<std::size_t>(seat)];
    }
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        platform::Log(L"lock-on: WARNING: target fields were asked for seat {}, which does not exist; a scratch record "
                      L"answered",
                      seat);
    }
    static SeatTargets scratch;
    scratch = {};
    return scratch;
}

/// A line of `level`.
void Note(const std::wstring& text, platform::Level level = platform::Level::Info) noexcept
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"lock-on: {}", text);
    }
}

/// True at 1, 10, 100, ...: counters logged without a budget of their own.
bool PowerOfTen(std::uint64_t count) noexcept
{
    while (count >= 10 && count % 10 == 0)
    {
        count /= 10;
    }
    return count == 1;
}

void Fail(const wchar_t* what) noexcept
{
    g_failed = true;
    platform::Log(L"lock-on: {} raised an exception; the further players' targeting is off this session", what);
}

Pair PairAt(std::uintptr_t address) noexcept
{
    return {ReadAt<std::uintptr_t>(address), ReadAt<std::uintptr_t>(address + 8)};
}

std::uintptr_t BattleSystem() noexcept
{
    return ReadAt<std::uintptr_t>(g_battleSystem);
}

std::uintptr_t TargetedKeyAddress() noexcept
{
    const auto manager = ReadAt<std::uintptr_t>(g_endUISystem + facts::kUIEnemyStatus);
    return manager != 0 ? manager + kTargetedKey : 0;
}

/// A weak reference the mod holds for a seat, given back.
void ReleasePair(Pair& pair) noexcept
{
    if (pair.controller != 0 && g_release != nullptr && !platform::CallGuarded(g_release, pair.controller))
    {
        Fail(L"a weak-reference release");
    }
    pair = {};
}

/// A seat's cursor, given back to the game's cursor manager.
void ReleaseCursor(std::uintptr_t& cursor) noexcept
{
    if (cursor != 0 && g_cursorRelease != nullptr)
    {
        const auto released = cursor;
        cursor              = 0;  // released widgets go to one shared reuse slot: never the seat's again
        if (!platform::CallGuarded(g_cursorRelease, reinterpret_cast<void*>(released)))
        {
            Fail(L"the cursor release");
        }
    }
    cursor = 0;
}

/// Whether the command controller in the UI system's slot has its target list open: the game's wrapper, or on
/// 1.005 its body with the slot the wrapper reads. False if neither is known or the call faulted.
bool TargetListActive(std::uint8_t& open) noexcept
{
    if (g_targetList != nullptr)
    {
        return platform::CallGuarded(g_targetList, open);
    }
    const auto controller =
        ReadAt<std::uintptr_t>(g_endUISystem != 0 ? g_endUISystem + facts::kUICommandController : 0);
    return g_targetListImpl != nullptr && controller != 0 &&
           platform::CallGuarded(g_targetListImpl, open, reinterpret_cast<void*>(controller));
}

/// The menu's target select is open or busy (its widgets are player 1's).
bool MenuTargetSelect() noexcept
{
    std::uint8_t open = 0, busy = 0;
    return !platform::CallGuarded(g_menuSelect, open) || open != 0 || !platform::CallGuarded(g_menuBusy, busy) || busy != 0;
}

/// The bar manager's targeted key is a seat's while it lives (the weak reference moves with it): one Exchange
/// range of platform/byte_scope.h over the pair, the seat's copy loaded in and read back at the end.
class TargetedKeyScope
{
public:
    explicit TargetedKeyScope(Pair& seatKey) noexcept : seatKey_(seatKey)
    {
        const std::uintptr_t address = TargetedKeyAddress();
        const Range          range{address, sizeof(Pair), Kind::Exchange};
        if (address == 0 || !context_.Define(std::span<const Range>(&range, 1)))
        {
            return;
        }
        std::memcpy(context_.Value(0).data(), &seatKey_, sizeof(Pair));
        held_ = context_.Open();
    }
    ~TargetedKeyScope()
    {
        if (held_)
        {
            context_.Close();
            std::memcpy(&seatKey_, context_.Value(0).data(), sizeof(Pair));
        }
    }
    TargetedKeyScope(const TargetedKeyScope&)            = delete;
    TargetedKeyScope& operator=(const TargetedKeyScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    Pair&                                seatKey_;
    platform::ByteStore<1, sizeof(Pair)> context_;
    bool                                 held_ = false;
};

void Refused(int seat, const wchar_t* why) noexcept
{
    ++g_refusals;
    if (g_refusals <= 5 || PowerOfTen(g_refusals))
    {
        try
        {
            Note(std::format(L"{}'s targeting skipped: {} ({} time(s))", players::SeatName(seat), why, g_refusals));
        }
        catch (...)
        {
        }
    }
}

/// A camera manager's LockOn module as the log shows it: its active bytes and cached target.
struct LockCamera
{
    bool           present = false;
    std::uint8_t   active[2]{};
    std::uintptr_t target = 0;
    bool operator==(const LockCamera&) const = default;
};

LockCamera LockCameraOf(void* controller) noexcept
{
    const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controller, facts::kControllerCameraManager));
    const auto op     = ReadAt<std::uintptr_t>(camera != 0 ? camera + kCameraManagerBattleOperator : 0);
    const auto module =
        reinterpret_cast<std::uintptr_t>(camera::camera_access::ModuleAt(reinterpret_cast<const void*>(op), kLockOnLayer));
    if (module == 0)
    {
        return {};
    }
    return {true, {ReadAt<std::uint8_t>(module + kModuleActive), ReadAt<std::uint8_t>(module + kModuleActive + 1)},
            ReadAt<std::uintptr_t>(module + kModuleTarget)};
}

std::wstring DescribeCamera(const LockCamera& camera)
{
    return camera.present ? std::format(L"{}{} target {:#x}", camera.active[0], camera.active[1], camera.target) : L"-";
}

/// What the log shows after each of a seat's runs, as raw values: compared, formatted on change.
struct Snapshot
{
    std::uint8_t   lock = 0, latch = 0;
    bool           resting = false;
    std::uintptr_t displayed = 0, targeted = 0, cursor = 0;
    LockCamera     cameras[2];  // player 1's and the seat's
    bool operator==(const Snapshot&) const = default;
};

std::array<Snapshot, players::kMaxPlayers> g_snapshots{};  // by seat; index 0 unused

void LogSeat(int seat) noexcept
{
    Snapshot& logged = g_snapshots[static_cast<std::size_t>(seat)];
    if (!platform::Enabled(platform::Level::Debug))
    {
        logged = Snapshot{};  // off: the next line starts fresh (as at start)
        return;
    }
    const SeatTargets& mine = SeatOf(seat);
    float              rest = 0.0f;
    std::memcpy(&rest, &mine.freeTime, sizeof(rest));
    const Snapshot now{mine.lock,
                       mine.latch,
                       rest > 0.0f,
                       mine.displayed.object,
                       mine.targetedKey.object,
                       mine.cursor,
                       {LockCameraOf(players::generation::PlayerOneController()), LockCameraOf(players::generation::ControllerOf(seat))}};
    if (now == logged)
    {
        return;
    }
    logged = now;
    try
    {
        Note(std::format(L"P{} lock {} latch {} rest {} displayed {:#x} targeted {:#x} cursor {:#x} | lock camera P1 {} "
                         L"P{} {} (runs {}, skipped {}, paused for the menu {}, panel requests skipped {})",
                         seat + 1, now.lock, now.latch, now.resting ? 1 : 0, now.displayed, now.targeted, now.cursor,
                         DescribeCamera(now.cameras[0]), seat + 1, DescribeCamera(now.cameras[1]), g_runs, g_refusals,
                         g_menuPauses, g_panelSkips + g_panelGate.skipped),
             platform::Level::Debug);
    }
    catch (...)
    {
    }
}

/// The game's targeting tick once more, for `seat`, inside the seat's scopes (S-TGT5 §4).
void SeatTargeting(int seat, TickFn engine, void* system, float deltaTime) noexcept
{
    if (!g_runReady || !g_enabled || g_failed || g_held)
    {
        return;
    }
    void* member = players::generation::MemberOf(seat);
    if (member == nullptr)
    {
        return;
    }
    if (MenuTargetSelect())
    {
        ++g_menuPauses;
        if (PowerOfTen(g_menuPauses))
        {
            try
            {
                Note(std::format(L"{}'s targeting waits while player 1 picks a target in the menu ({} time(s))",
                                 players::SeatName(seat), g_menuPauses));
            }
            catch (...)
            {
            }
        }
        return;
    }
    seat_ladder::Args args;
    args.seat = seat;
    if (!seat_ladder::FillMember(args, member))
    {
        return Refused(seat, L"the member could not be read");
    }
    // The seat's UI context, as around its command controller's own tick: the targeting reads the command controller
    // (EndUISystem[23]) and, for an open target list, the list's selected row, which the list answers from the focus
    // path (RE-p2-target-select-views.md §9). The controller slot alone would read player 1's focus path, find no row
    // and clear the seat's target.
    args.controller = command_menu::Controller(seat);
    if (args.controller == nullptr)
    {
        return Refused(seat, L"the command controller does not exist");
    }
    seat_ladder::SeatScopes scopes(seat_ladder::Focus | seat_ladder::Sticks | seat_ladder::Primary |
                                       seat_ladder::Buttons | seat_ladder::Targets | seat_ladder::Leader |
                                       seat_ladder::LeaderType,
                                   args);
    if (!scopes.Held())
    {
        return Refused(seat, scopes.Refusal());
    }
    SeatTargets&           mine = SeatOf(seat);
    const TargetedKeyScope targeted(mine.targetedKey);
    if (!targeted.Held())
    {
        return Refused(seat, L"the targeted key");
    }
    g_run            = true;
    g_panelGate.skip = 1;
    const bool ok    = platform::CallGuarded(engine, system, deltaTime);
    g_panelGate.skip = 0;
    g_run            = false;
    ++g_runs;
    if (!ok)
    {
        // What it holds may be mid-change: leaked, never put back.
        scopes.Get<seat_ladder::FocusPart>()->scope.Abandon();
        Fail(L"a further player's targeting tick");
    }
    // The log: the seat's target list, read with the seat's controller still in the slot.
    std::uint8_t open = 0;
    if (TargetListActive(open) && (open != 0) != mine.listOpen)
    {
        mine.listOpen = open != 0;
        if (mine.listOpen)
        {
            ++mine.listOpenings;
        }
        try
        {
            if (platform::Enabled(platform::Level::Debug))
            {
                Note(std::format(L"{}'s command target list {} (displayed {:#x}, cursor {:#x}; player 1's cursor {:#x})",
                                 players::SeatName(seat), mine.listOpen ? L"OPENED" : L"closed",
                                 ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(system) + kDisplayed),
                                 ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(system) + kCursor),
                                 scopes.Get<seat_ladder::TargetsPart>()->scope.OtherCursor()),
                     platform::Level::Debug);
            }
        }
        catch (...)
        {
        }
    }
}

/// Replaces the battle tick's calls to the targeting tick: player 1's run (on pad 1's own right
/// stick while a seat plays), then each seat's.
void Targeting(void* system, float deltaTime) noexcept
{
    const auto engine = reinterpret_cast<TickFn>(g_tickTargetingCalls[0].Original());
    if (!players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        return engine(system, deltaTime);
    }
    if (g_held)
    {
        Note(L"WARNING: the targeting tick ran inside a further player's target scope; their run skipped");
        return engine(system, deltaTime);
    }
    if (input::pad_sticks::Enabled() && input::pad_sticks::PartActs(input::pad_sticks::kPartPlayerOneMove))
    {
        const input::pad_sticks::StickScope sticks(input::pad_sticks::ControllerIdOf(players::generation::PlayerOneController()));
        static bool                  refusedLogged = false;
        if (!sticks.Held() && !refusedLogged)
        {
            refusedLogged = true;  // another scope was open: this tick reads whatever stick that scope holds
            Note(L"player 1's targeting ran without pad 1's own stick (another stick scope was open)");
        }
        engine(system, deltaTime);
    }
    else
    {
        engine(system, deltaTime);
    }
    // Player 1's own target and lock, on change, beside player 2's in the same log.
    static std::uintptr_t loggedTarget = 0;
    static std::uint8_t   loggedLock   = 0;
    if (platform::Enabled(platform::Level::Debug))
    {
        const auto base   = reinterpret_cast<std::uintptr_t>(system);
        const auto target = ReadAt<std::uintptr_t>(base + kDisplayed);
        const auto lock   = ReadAt<std::uint8_t>(base + kLock);
        if (target != loggedTarget || lock != loggedLock)
        {
            loggedTarget = target;
            loggedLock   = lock;
            try
            {
                Note(std::format(L"player 1's target {:#x} lock {}", target, lock), platform::Level::Debug);
            }
            catch (...)
            {
            }
        }
    }
    // The game's tick once more for each further player, in seat order, each run's scopes closed before the next's. A
    // third run is as safe as the second (RE-seats-step6-battle.md §1): the one timer the tick advances by frame time,
    // the lock camera's rest (+0x90), is in each seat's copied fields, and its shared writes are the ones already gated.
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
        {
            SeatTargeting(seat, engine, system, deltaTime);
            LogSeat(seat);
        }
    }
}

/// Replaces the tick's request for player 1's target panel (the new target): skipped in player 2's
/// run, its weak reference released as the request would release it. (The "old target cleared"
/// request is gated in its own chunk: battle/panel_gate_code.h.)
void PanelSet(void* weakPtr, float x, float y) noexcept
{
    if (!g_run)
    {
        return reinterpret_cast<PanelSetFn>(g_panelSetCall.Original())(weakPtr, x, y);
    }
    ++g_panelSkips;
    platform::CallGuarded(g_weakRelease, weakPtr);
}

/// 1.005: replaces the tick's request for player 1's panel "old target cleared", whose call fits one block there
/// (1.004 gates its chunk instead, panel_gate_code.h): skipped in player 2's run, its weak reference released as
/// the request would release it at its end. The same net effect as the gate: no request, no reference left.
void PanelCleared(void* weakPtr) noexcept
{
    if (!g_run)
    {
        return reinterpret_cast<WeakReleaseFn>(g_panelClearedCall.Original())(weakPtr);
    }
    ++g_panelGate.skipped;
    platform::CallGuarded(g_weakRelease, weakPtr);
}

/// Replaces the tick's write of the marker list's target owner name: skipped in player 2's run.
void MarkerName(void* owner) noexcept
{
    if (!g_run)
    {
        reinterpret_cast<MarkerNameFn>(g_markerNameCall.Original())(owner);
    }
}

/// A seat's displayed target, targeted key and cursor given back; its lock, switch latch, rest timer and
/// trace handle cleared (outside a TargetScope).
void ForgetTargets(int seat) noexcept
{
    SeatTargets& mine = SeatOf(seat);
    ReleasePair(mine.displayed);
    ReleasePair(mine.targetedKey);
    ReleaseCursor(mine.cursor);
    mine.lock     = 0;
    mine.latch    = 0;
    mine.freeTime = 0;
    mine.trace    = 0;
}

/// A seat's lock ends with the battle, as the game's teardown ends player 1's: the lock camera
/// off (inside the seat's member tick, its camera manager is the primary one), the displayed target,
/// the targeted key and the cursor given back, the lock and the rest timer cleared.
void EndOfBattle(int seat) noexcept
{
    std::uint8_t on = 0;
    if (platform::CallGuarded(g_moduleOn, on, kLockOnLayer) && on != 0 &&
        !platform::CallGuarded(g_deactivate, kLockOnLayer))
    {
        Fail(L"the lock camera call");
    }
    const std::uint8_t lock = SeatOf(seat).lock;
    ForgetTargets(seat);
    try
    {
        Note(std::format(L"battle over: {}'s lock {} cleared", players::SeatName(seat), lock));
    }
    catch (...)
    {
    }
}

bool Redirect(const platform::GameImage& image, platform::CallRedirect& call, std::uintptr_t site,
              const platform::FunctionFact& fact, std::uintptr_t replacement) noexcept
{
    const auto target = platform::FindFunction(image, fact);
    const auto live   = image.Rebase(site, 5);
    return target && live && g_placement.Place(call, *live, *target, replacement);
}

/// The "old target cleared" request kept from player 1's panel in player 2's run. On 1.005 its call fits one
/// block and is redirected (PanelCleared); on the research build its chunk is gated
/// (battle/panel_gate_code.h),
/// once the chunk is the one the research read: its call still names the request, and the gated instruction matches.
bool InstallPanelGate(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        return platform::FindFunction(image, kPanelClearedCall) &&
               Redirect(image, g_panelClearedCall, kPanelClearedCall.address, kPanelCleared,
                        reinterpret_cast<std::uintptr_t>(&PanelCleared));
    }
    const auto request = platform::FindFunction(image, kPanelCleared);
    const auto site    = image.Rebase(kPanelClearedSite, sizeof(kPanelGateSiteBytes));
    const auto resume  = image.Rebase(kPanelClearedResume);
    const auto skipTo  = image.Rebase(kPanelClearedSkipTo);
    if (!request || !site || !resume || !skipTo || !platform::FindFunction(image, kPanelClearedCall))
    {
        return false;
    }
    const auto code = PanelGateCode(g_panelGate, *skipTo);
    return g_panelClearedGate.Install(*site, kPanelGateSiteBytes, code, *resume);
}

void RestoreCall(platform::CallRedirect& call) noexcept
{
    if (call.IsInstalled() && !call.Restore())
    {
        platform::Log(L"lock-on: a call was changed after us; left in place (it passes through)");
    }
}

bool PlaceSites(const platform::GameImage& image) noexcept;

}  // namespace

TargetScope::TargetScope(int seat) noexcept : seat_(seat)
{
    if (!players::SeatIndex(seat) || !g_installed || g_failed || g_held || !players::generation::OnGameThread())
    {
        return;
    }
    const SeatTargets& mine = SeatOf(seat);
    const std::uintptr_t system = BattleSystem();
    if (system == 0 || !facts::InBattle())
    {
        return;
    }
    // The seat's fields in and player 1's saved: five Exchange ranges, then the lock byte.
    const Range ranges[] = {{system + kCursor, sizeof(mine.cursor), Kind::Exchange},
                            {system + kDisplayed, sizeof(Pair), Kind::Exchange},
                            {system + kTrace, sizeof(mine.trace), Kind::Exchange},
                            {system + kFreeTime, sizeof(mine.freeTime), Kind::Exchange},
                            {system + kLatch, sizeof(mine.latch), Kind::Exchange}};
    if (!fields_.Define(ranges))
    {
        return;
    }
    std::memcpy(fields_.Value(0).data(), &mine.cursor, sizeof(mine.cursor));
    std::memcpy(fields_.Value(1).data(), &mine.displayed, sizeof(Pair));
    std::memcpy(fields_.Value(2).data(), &mine.trace, sizeof(mine.trace));
    std::memcpy(fields_.Value(3).data(), &mine.freeTime, sizeof(mine.freeTime));
    fields_.Value(4)[0] = mine.latch;
    cursor_             = ReadAt<std::uintptr_t>(system + kCursor);
    lock_               = ReadAt<std::uint8_t>(system + kLock);
    seatLock_           = mine.lock;
    if (!fields_.Open())
    {
        return;
    }
    if (!WriteAt(system + kLock, mine.lock))
    {
        fields_.Close();  // the fields are the game's heap: player 1's back (the seat's copies as they were)
        return;
    }
    system_ = system;
    g_held  = true;
}

TargetScope::~TargetScope()
{
    if (system_ == 0)
    {
        return;
    }
    SeatTargets& mine = SeatOf(seat_);
    mine.lock = ReadAt<std::uint8_t>(system_ + kLock);  // the seat's lock as the game left it
    fields_.Close();                                    // the seat's five fields read back, player 1's put back
    std::memcpy(&mine.cursor, fields_.Value(0).data(), sizeof(mine.cursor));
    std::memcpy(&mine.displayed, fields_.Value(1).data(), sizeof(Pair));
    std::memcpy(&mine.trace, fields_.Value(2).data(), sizeof(mine.trace));
    std::memcpy(&mine.freeTime, fields_.Value(3).data(), sizeof(mine.freeTime));
    mine.latch    = fields_.Value(4)[0];
    bool restored = !fields_.Faulted();
    // The seat's toggle went through the game's lock setter, which also set Global.LockOn to the seat's
    // lock: player 1's lock goes back through the same setter, which sets the parameter back. The setter
    // touches only the lock byte and the parameter, so the scopes still open around this one (the seat
    // as the primary controller, its pad's sticks) do not matter to it.
    std::uint8_t previous = 0;
    if (mine.lock != seatLock_ && g_setLock != nullptr &&
        platform::CallGuarded(g_setLock, previous, reinterpret_cast<void*>(system_), lock_))
    {
        ++g_lockRestores;
    }
    else
    {
        restored = WriteAt(system_ + kLock, lock_) && restored;
    }
    g_held = false;
    if (!restored)
    {
        Fail(L"putting player 1's target fields back");  // the seats' stay off: the next scope would read them as player 1's
    }
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    g_setLock       = reinterpret_cast<SetLockFn>(find(kSetLock));
    g_menuSelect    = reinterpret_cast<TestFn>(find(kMenuSelect));
    g_menuBusy      = reinterpret_cast<TestFn>(find(kMenuBusy));
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        g_targetListImpl = reinterpret_cast<ListImplFn>(find(kTargetListImpl));  // the wrapper was inlined
    }
    else
    {
        g_targetList = reinterpret_cast<TestFn>(find(kTargetList));
    }
    g_release       = reinterpret_cast<ReleaseFn>(find(facts::kReleaseWeakReference));
    g_weakRelease   = reinterpret_cast<WeakReleaseFn>(find(facts::kWeakPtrRelease));
    g_cursorRelease = reinterpret_cast<CursorFn>(find(kCursorRelease));
    g_deactivate    = reinterpret_cast<LayerFn>(find(kDeactivate));
    g_moduleOn      = reinterpret_cast<LayerTestFn>(find(kModuleOn));
    const auto system = image.Rebase(kBattleStateGlobal, sizeof(void*));
    const auto ui     = image.Rebase(facts::kEndUISystem, facts::kUIEnemyStatus + sizeof(void*));
    g_battleSystem    = system ? *system : 0;
    g_endUISystem     = ui ? *ui : 0;
    g_installed = g_setLock && facts::InBattleKnown() && g_release && g_cursorRelease && g_deactivate && g_moduleOn &&
                  system;
    if (!g_installed)
    {
        platform::Log(L"lock-on: UNAVAILABLE (a check failed); the further players have no targeting of their own");
        return false;
    }
    // Both targeting calls and the "set" call cross a 16-byte block in 1.005: every site is placed from the game
    // thread, between its ticks (platform/deferred_sites.h).
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"lock-on: {}",
                      placed ? L"the targeting calls are placed from the game thread (1.005: they cross a 16-byte "
                               L"block)"
                             : L"the further players' targeting run UNAVAILABLE (no room to place its calls from the game "
                               L"thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_installed   = false;  // a TargetScope opened from here on holds nothing
    g_runReady    = false;
    g_placement.Cancel();  // a placement still queued does nothing
    for (auto& call : g_tickTargetingCalls)
    {
        RestoreCall(call);
    }
    if (g_panelClearedGate.IsInstalled() && !g_panelClearedGate.Restore())
    {
        platform::Log(L"lock-on: the panel gate was changed after us; left in place (it passes through while unset)");
    }
    RestoreCall(g_panelClearedCall);
    RestoreCall(g_panelSetCall);
    RestoreCall(g_markerNameCall);
}

namespace {

/// The call sites: both targeting calls (or neither), then the tick's three shared writes skipped in player 2's
/// run: all of those, or player 2 gets no run (a partial set would leak player 2's targeting into player 1's panel
/// or markers); the calls alone still keep player 1's targeting on pad 1's stick.
bool PlaceSites(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    const std::uintptr_t tick = find(kTickTargeting);
    bool                 ok   = tick != 0 && g_menuSelect && g_menuBusy && g_weakRelease && g_endUISystem != 0;
    for (std::size_t i = 0; ok && i < 2; ++i)
    {
        const auto site = image.Rebase(kTickTargetingCalls[i], 5);
        ok = site && g_placement.Place(g_tickTargetingCalls[i], *site, tick,
                                       reinterpret_cast<std::uintptr_t>(&Targeting));
    }
    if (!ok)
    {
        for (auto& call : g_tickTargetingCalls)
        {
            RestoreCall(call);
        }
    }
    // Each part logged, so a failed check names itself.
    const bool calls   = ok;
    const bool cleared = calls && InstallPanelGate(image);
    const bool set     = cleared && Redirect(image, g_panelSetCall, kPanelSetCall, kPanelSet, reinterpret_cast<std::uintptr_t>(&PanelSet));
    const bool marker  = set && Redirect(image, g_markerNameCall, kMarkerNameCall, kMarkerName, reinterpret_cast<std::uintptr_t>(&MarkerName));
    ok                 = marker;
    if (!ok)
    {
        // Player 2's run stays off; the targeting calls, if they installed, keep player 1's on pad 1.
        if (g_panelClearedGate.IsInstalled())
        {
            g_panelClearedGate.Restore();
        }
        RestoreCall(g_panelClearedCall);
        RestoreCall(g_panelSetCall);
        RestoreCall(g_markerNameCall);
    }
    g_runReady = ok;
    platform::Log(L"lock-on: {}", ok ? L"the further players' targeting is the game's own tick, run again in each one's scopes (logged)"
                                     : L"the further players' targeting run UNAVAILABLE (a check failed); they have no lock-on");
    if (!ok)
    {
        platform::Log(L"lock-on: checks: targeting calls {}, panel 'cleared' {} {}, panel 'set' call {}, marker name call {}",
                      calls ? L"ok" : L"FAILED", g_placement.FromGameThread() ? L"call" : L"gate",
                      cleared ? L"ok" : (calls ? L"FAILED" : L"not tried"),
                      set ? L"ok" : (cleared ? L"FAILED" : L"not tried"), marker ? L"ok" : (set ? L"FAILED" : L"not tried"));
    }
    return ok;
}

}  // namespace

void AfterBattle(int seat) noexcept
{
    if (!players::SeatIndex(seat) || !g_installed || g_held || !players::generation::OnGameThread())
    {
        return;
    }
    const SeatTargets& mine = SeatOf(seat);
    if ((mine.lock != 0 || mine.displayed.controller != 0 || mine.targetedKey.controller != 0 || mine.cursor != 0 ||
         mine.freeTime != 0) &&
        !facts::InBattle())
    {
        EndOfBattle(seat);
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"lock-on: the further players' targeting run {}", on ? L"on" : L"off");
}

bool Enabled() noexcept
{
    return g_runReady && g_enabled && !g_failed;
}

void SeatOut(int seat) noexcept
{
    if (!g_held && players::SeatIndex(seat))
    {
        ForgetTargets(seat);
    }
}

bool Held() noexcept
{
    return g_held;
}

std::uintptr_t DisplayedObject(int seat) noexcept
{
    return players::SeatIndex(seat) ? SeatOf(seat).displayed.object : 0;
}

void Displayed(int seat, std::uintptr_t& object, std::uintptr_t& controller) noexcept
{
    const Pair pair = players::SeatIndex(seat) ? SeatOf(seat).displayed : Pair{};
    object          = pair.object;
    controller      = pair.controller;
}

void* Cursor(int seat) noexcept
{
    return players::SeatIndex(seat) ? reinterpret_cast<void*>(SeatOf(seat).cursor) : nullptr;
}

bool TargetListOpen(int seat) noexcept
{
    return players::SeatIndex(seat) && SeatOf(seat).listOpen;
}

std::uint32_t TargetListOpenings(int seat) noexcept
{
    return players::SeatIndex(seat) ? SeatOf(seat).listOpenings : 0;
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"battle-mask",
                       .shape = control::Shape::Integer,
                       .max   = 511,
                       .help  = L"diagnosis: bit 8 runs the further players' targeting (the game's targeting tick for each of "
                                L"them); the other bits are retired and ignored; on at start",
                       .set   = [](const Value& v) {
                           // Bit 8 is player 2's targeting run. The other bits (1, 2, 4, 16: the direct actions;
                           // 32, 64, 128, 256: the mod's own lock, switching, lock camera and cursor) are retired:
                           // the game's own handler and targeting tick do those for player 2.
                           SetEnabled((std::get<std::int64_t>(v) & 8) != 0);
                           return Outcome{true, L"battle mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Enabled() ? 8 : 0}}; }});
}

#endif

}  // namespace lss::battle::lock_on
