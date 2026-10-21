#include "player_two/lock_on.h"

#include "facts/controller.h"
#include "facts/hud.h"
#include "facts/in_battle.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/command_menu_two.h"
#include "player_two/focus_scope.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/panel_gate_code.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"

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

namespace lss::player_two::lock_on {

namespace {

// Facts read from the database (RE-p2-targeting-scoped-tick.md §4.3, RE-p2-lock-on.md §R.4; this
// build). Each function's first bytes, through its stack probe, are checked before it is used.
constexpr std::uint8_t kTickTargetingBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0xAC, 0x74, 0x01};
constexpr std::uint8_t kSetLockBytes[]       = {0xB8, 0x98, 0x00, 0x00, 0x00, 0xE8, 0xCE, 0x3B, 0x81, 0x00,
                                                0x48, 0x2B, 0xE0, 0x8A, 0x81, 0x8C, 0x00, 0x00, 0x00};
constexpr std::uint8_t kMenuSelectBytes[]    = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0xF9, 0x41, 0x01};
constexpr std::uint8_t kMenuBusyBytes[]      = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xFE, 0xF9, 0x41, 0x01,
                                                0x48, 0x2B, 0xE0, 0xE8, 0x06, 0x74, 0xB7, 0xFF};
constexpr std::uint8_t kReleaseBytes[]       = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x26, 0x74, 0x82, 0x01};
constexpr std::uint8_t kWeakReleaseBytes[]   = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x46, 0x74, 0x82, 0x01,
                                                0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x49, 0x08};
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
constexpr platform::FunctionFact kRelease{L"TRefCtrlTS__ReleaseWeakRef", 0x140887B00, kReleaseBytes};
constexpr platform::FunctionFact kWeakRelease{L"TWeakPtrTS__Release", 0x140887AE0, kWeakReleaseBytes};
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
constexpr std::uintptr_t kPanelClearedCall     = 0x14295F0BE;  // the request's call, checked: E8 to EndUI4_QueueTargetCleared
constexpr std::uint8_t   kPanelClearedCallBytes[] = {0xE8, 0xC1, 0x79, 0x07, 0x01};
constexpr std::uintptr_t kPanelClearedSkipTo   = 0x14295F0C3;  // after the request's call
constexpr std::uintptr_t kPanelSetCall         = 0x14295F139;  // crosses a block on 1.005 (placed from the game thread)
constexpr std::uintptr_t kMarkerNameCall       = 0x140C8F9C9;

// The battle system (a pointer global) and its target fields (§R.1, §9; S-TGT5 §5).
constexpr std::uintptr_t kBattleSystem = 0x14939BC58;
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
constexpr std::uint32_t  kBattleOperator = 0x2CA8;
constexpr std::uint32_t  kModules        = 0x6E0;
constexpr std::uint32_t  kModuleEntry    = 16;
constexpr std::uint32_t  kModuleActive   = 0x44;
constexpr std::uint32_t  kModuleTarget   = 0x3F8;


struct Pair  // a TWeakPtr as the game holds it
{
    std::uintptr_t object     = 0;
    std::uintptr_t controller = 0;
    bool operator==(const Pair&) const = default;
};

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

// Game thread. Player 2's copies of the battle system's fields (the displayed target owns one weak
// reference) and of the bar manager's targeted key (one weak reference), read back at the end of
// each scope.
Pair          g_displayed;
Pair          g_targetedKey;
std::uintptr_t g_cursor   = 0;
std::uint64_t g_trace     = 0;
std::uint32_t g_freeTime  = 0;
std::uint8_t  g_lock      = 0;
std::uint8_t  g_latch     = 0;
bool          g_held      = false;
bool          g_run       = false;  // inside player 2's targeting run: the shared writes are skipped
PanelGate     g_panelGate;          // the same, for the "cleared" request (player_two/panel_gate_code.h)
bool          g_enabled   = true;
bool          g_listOpen  = false;  // player 2's command target list, as last seen in player 2's run
std::uint32_t g_listOpenings = 0;
bool          g_failed    = false;
std::uint64_t g_runs = 0, g_refusals = 0, g_menuPauses = 0, g_panelSkips = 0, g_lockRestores = 0;

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
    platform::Log(L"lock-on: {} raised an exception; player 2's targeting is off this session", what);
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

/// A weak reference the mod holds for player 2, given back.
void ReleasePair(Pair& pair) noexcept
{
    if (pair.controller != 0 && g_release != nullptr && !platform::CallGuarded(g_release, pair.controller))
    {
        Fail(L"a weak-reference release");
    }
    pair = {};
}

/// Player 2's cursor, given back to the game's cursor manager.
void ReleaseCursor() noexcept
{
    if (g_cursor != 0 && g_cursorRelease != nullptr)
    {
        const auto released = g_cursor;
        g_cursor            = 0;  // released widgets go to one shared reuse slot: never player 2's again
        if (!platform::CallGuarded(g_cursorRelease, reinterpret_cast<void*>(released)))
        {
            Fail(L"the cursor release");
        }
    }
    g_cursor = 0;
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

/// The bar manager's targeted key is player 2's while it lives (the weak reference moves with it).
class TargetedKeyScope
{
public:
    TargetedKeyScope() noexcept : address_(TargetedKeyAddress())
    {
        if (address_ == 0)
        {
            return;
        }
        saved_ = PairAt(address_);
        if (!WriteAt(address_, g_targetedKey))
        {
            WriteAt(address_, saved_);
            address_ = 0;
        }
    }
    ~TargetedKeyScope()
    {
        if (address_ != 0)
        {
            g_targetedKey = PairAt(address_);
            WriteAt(address_, saved_);
        }
    }
    TargetedKeyScope(const TargetedKeyScope&)            = delete;
    TargetedKeyScope& operator=(const TargetedKeyScope&) = delete;

    bool Held() const noexcept { return address_ != 0; }

private:
    std::uintptr_t address_ = 0;
    Pair           saved_;
};

void Refused(const wchar_t* why) noexcept
{
    ++g_refusals;
    if (g_refusals <= 5 || PowerOfTen(g_refusals))
    {
        try
        {
            Note(std::format(L"player 2's targeting skipped: {} ({} time(s))", why, g_refusals));
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
    const auto op     = ReadAt<std::uintptr_t>(camera != 0 ? camera + kBattleOperator : 0);
    const auto table  = ReadAt<std::uintptr_t>(op != 0 ? op + kModules : 0);
    const auto module = ReadAt<std::uintptr_t>(table != 0 ? table + kLockOnLayer * kModuleEntry : 0);
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

/// What the log shows after each of player 2's runs, as raw values: compared, formatted on change.
struct Snapshot
{
    std::uint8_t   lock = 0, latch = 0;
    bool           resting = false;
    std::uintptr_t displayed = 0, targeted = 0, cursor = 0;
    LockCamera     cameras[2];
    bool operator==(const Snapshot&) const = default;
};

Snapshot g_snapshot;

void LogPlayerTwo() noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_snapshot = Snapshot{};  // off: the next line starts fresh (as at start)
        return;
    }
    float rest = 0.0f;
    std::memcpy(&rest, &g_freeTime, sizeof(rest));
    const Snapshot now{g_lock,
                       g_latch,
                       rest > 0.0f,
                       g_displayed.object,
                       g_targetedKey.object,
                       g_cursor,
                       {LockCameraOf(generation::PlayerOneController()), LockCameraOf(generation::PlayerTwoController())}};
    if (now == g_snapshot)
    {
        return;
    }
    g_snapshot = now;
    try
    {
        Note(std::format(L"P2 lock {} latch {} rest {} displayed {:#x} targeted {:#x} cursor {:#x} | lock camera P1 {} P2 {} "
                         L"(runs {}, skipped {}, paused for the menu {}, panel requests skipped {})",
                         now.lock, now.latch, now.resting ? 1 : 0, now.displayed, now.targeted, now.cursor,
                         DescribeCamera(now.cameras[0]), DescribeCamera(now.cameras[1]), g_runs, g_refusals, g_menuPauses,
                         g_panelSkips + g_panelGate.skipped),
             platform::Level::Debug);
    }
    catch (...)
    {
    }
}

/// The game's targeting tick once more, for player 2, inside player 2's scopes (S-TGT5 §4).
void PlayerTwoTargeting(TickFn engine, void* system, float deltaTime) noexcept
{
    if (!g_runReady || !g_enabled || g_failed || g_held)
    {
        return;
    }
    void* member = generation::Member();
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
                Note(std::format(L"player 2's targeting waits while player 1 picks a target in the menu ({} time(s))",
                                 g_menuPauses));
            }
            catch (...)
            {
            }
        }
        return;
    }
    const auto memberType = party_member::PlayerTypeOf(member);
    const auto memberWeak = leader_scope::WeakOf(member);
    if (!memberType || memberWeak == 0)
    {
        return Refused(L"player 2's member could not be read");
    }
    // Player 2's UI context, as around player 2's command controller's own tick: the targeting reads the
    // command controller (EndUISystem[23]) and, for an open target list, the list's selected row, which
    // the list answers from the focus path (RE-p2-target-select-views.md §9). The controller slot alone
    // would read player 1's focus path, find no row and clear player 2's target.
    void* controller = command_menu_two::Controller();
    if (controller == nullptr)
    {
        return Refused(L"player 2's command controller does not exist");
    }
    focus_scope::FocusScope focus(controller);
    if (!focus.Entered())
    {
        return Refused(focus.Refusal());
    }
    const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(generation::PlayerTwoController()));
    const primary_controller::Scope primary;
    if (!sticks.Held() || !primary.Held())
    {
        return Refused(L"pad 2's sticks or the primary controller");
    }
    const pad_buttons::ButtonScope      buttons;
    const TargetScope                   targets;
    const leader_scope::LeaderScope     leader(memberWeak);
    const leader_scope::LeaderTypeScope type(*memberType);
    const TargetedKeyScope              targeted;
    if (!buttons.Held() || !targets.Held() || !leader.Held() || !type.Held() || !targeted.Held())
    {
        return Refused(L"pad 2's buttons, the target fields, the party leader or the targeted key");
    }
    g_run            = true;
    g_panelGate.skip = 1;
    const bool ok    = platform::CallGuarded(engine, system, deltaTime);
    g_panelGate.skip = 0;
    g_run            = false;
    ++g_runs;
    if (!ok)
    {
        focus.Abandon();  // what the scope holds may be mid-change: leaked, never put back
        Fail(L"player 2's targeting tick");
    }
    // The log: player 2's target list, read with player 2's controller still in the slot.
    std::uint8_t open = 0;
    if (TargetListActive(open) && (open != 0) != g_listOpen)
    {
        g_listOpen = open != 0;
        if (g_listOpen)
        {
            ++g_listOpenings;
        }
        try
        {
            if (platform::Enabled(platform::Level::Debug))
            {
                Note(std::format(L"player 2's command target list {} (displayed {:#x}, cursor {:#x}; player 1's cursor {:#x})",
                                 g_listOpen ? L"OPENED" : L"closed",
                                 ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(system) + kDisplayed),
                                 ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(system) + kCursor),
                                 targets.OtherCursor()),
                     platform::Level::Debug);
            }
        }
        catch (...)
        {
        }
    }
}

/// Replaces the battle tick's calls to the targeting tick: player 1's run (on pad 1's own right
/// stick while player 2 plays), then player 2's.
void Targeting(void* system, float deltaTime) noexcept
{
    const auto engine = reinterpret_cast<TickFn>(g_tickTargetingCalls[0].Original());
    if (!generation::OnGameThread() || !generation::Active())
    {
        return engine(system, deltaTime);
    }
    if (g_held)
    {
        Note(L"WARNING: the targeting tick ran inside player 2's target scope; player 2's run skipped");
        return engine(system, deltaTime);
    }
    if (pad_sticks::Enabled() && pad_sticks::PartActs(pad_sticks::kPartPlayerOneMove))
    {
        const pad_sticks::StickScope sticks(pad_sticks::ControllerIdOf(generation::PlayerOneController()));
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
    PlayerTwoTargeting(engine, system, deltaTime);
    LogPlayerTwo();
}

/// Replaces the tick's request for player 1's target panel (the new target): skipped in player 2's
/// run, its weak reference released as the request would release it. (The "old target cleared"
/// request is gated in its own chunk: player_two/panel_gate_code.h.)
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

/// Player 2's lock ends with the battle, as the game's teardown ends player 1's: the lock camera
/// off (inside the member tick, player 2's camera manager is the primary one), the displayed target,
/// the targeted key and the cursor given back, the lock and the rest timer cleared.
void EndOfBattle() noexcept
{
    std::uint8_t on = 0;
    if (platform::CallGuarded(g_moduleOn, on, kLockOnLayer) && on != 0 &&
        !platform::CallGuarded(g_deactivate, kLockOnLayer))
    {
        Fail(L"the lock camera call");
    }
    ReleasePair(g_displayed);
    ReleasePair(g_targetedKey);
    ReleaseCursor();
    try
    {
        Note(std::format(L"battle over: player 2's lock {} cleared", g_lock));
    }
    catch (...)
    {
    }
    g_lock     = 0;
    g_latch    = 0;
    g_freeTime = 0;
    g_trace    = 0;
}

bool Redirect(const platform::GameImage& image, platform::CallRedirect& call, std::uintptr_t site,
              const platform::FunctionFact& fact, std::uintptr_t replacement) noexcept
{
    const auto target = platform::FindFunction(image, fact);
    const auto live   = image.Rebase(site, 5);
    return target && live && g_placement.Place(call, *live, *target, replacement);
}

/// The "old target cleared" request kept from player 1's panel in player 2's run. On 1.005 its call fits one
/// block and is redirected (PanelCleared); on the research build its chunk is gated (player_two/panel_gate_code.h),
/// once the chunk is the one the research read: its call still names the request, and the gated instruction matches.
bool InstallPanelGate(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        return image.BytesMatch(kPanelClearedCall, kPanelClearedCallBytes) &&
               Redirect(image, g_panelClearedCall, kPanelClearedCall, kPanelCleared, reinterpret_cast<std::uintptr_t>(&PanelCleared));
    }
    const auto request = platform::FindFunction(image, kPanelCleared);
    const auto site    = image.Rebase(kPanelClearedSite, sizeof(kPanelGateSiteBytes));
    const auto resume  = image.Rebase(kPanelClearedResume);
    const auto skipTo  = image.Rebase(kPanelClearedSkipTo);
    if (!request || !site || !resume || !skipTo || !image.BytesMatch(kPanelClearedCall, kPanelClearedCallBytes))
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

TargetScope::TargetScope() noexcept
{
    if (!g_installed || g_failed || g_held || !generation::OnGameThread())
    {
        return;
    }
    const std::uintptr_t system = BattleSystem();
    if (system == 0 || !facts::InBattle())
    {
        return;
    }
    const Pair saved = PairAt(system + kDisplayed);
    cursor_          = ReadAt<std::uintptr_t>(system + kCursor);
    object_          = saved.object;
    controller_      = saved.controller;
    trace_           = ReadAt<std::uint64_t>(system + kTrace);
    freeTime_        = ReadAt<std::uint32_t>(system + kFreeTime);
    lock_            = ReadAt<std::uint8_t>(system + kLock);
    latch_           = ReadAt<std::uint8_t>(system + kLatch);
    twoLock_         = g_lock;
    if (!WriteAt(system + kCursor, g_cursor) || !WriteAt(system + kDisplayed, g_displayed) ||
        !WriteAt(system + kTrace, g_trace) || !WriteAt(system + kFreeTime, g_freeTime) ||
        !WriteAt(system + kLock, g_lock) || !WriteAt(system + kLatch, g_latch))
    {
        WriteAt(system + kCursor, cursor_);  // the fields are the game's heap: put back what was written
        WriteAt(system + kDisplayed, saved);
        WriteAt(system + kTrace, trace_);
        WriteAt(system + kFreeTime, freeTime_);
        WriteAt(system + kLock, lock_);
        WriteAt(system + kLatch, latch_);
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
    g_cursor    = ReadAt<std::uintptr_t>(system_ + kCursor);
    g_displayed = PairAt(system_ + kDisplayed);
    g_trace     = ReadAt<std::uint64_t>(system_ + kTrace);
    g_freeTime  = ReadAt<std::uint32_t>(system_ + kFreeTime);
    g_lock      = ReadAt<std::uint8_t>(system_ + kLock);
    g_latch     = ReadAt<std::uint8_t>(system_ + kLatch);
    bool restored = WriteAt(system_ + kCursor, cursor_) && WriteAt(system_ + kDisplayed, Pair{object_, controller_}) &&
                    WriteAt(system_ + kTrace, trace_) && WriteAt(system_ + kFreeTime, freeTime_) &&
                    WriteAt(system_ + kLatch, latch_);
    // Player 2's toggle went through the game's lock setter, which also set Global.LockOn to player 2's
    // lock: player 1's lock goes back through the same setter, which sets the parameter back. The setter
    // touches only the lock byte and the parameter, so the scopes still open around this one (player 2
    // as the primary controller, pad 2's sticks) do not matter to it.
    std::uint8_t previous = 0;
    if (g_lock != twoLock_ && g_setLock != nullptr &&
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
        Fail(L"putting player 1's target fields back");  // player 2's stay off: the next scope would read them as player 1's
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
    g_release       = reinterpret_cast<ReleaseFn>(find(kRelease));
    g_weakRelease   = reinterpret_cast<WeakReleaseFn>(find(kWeakRelease));
    g_cursorRelease = reinterpret_cast<CursorFn>(find(kCursorRelease));
    g_deactivate    = reinterpret_cast<LayerFn>(find(kDeactivate));
    g_moduleOn      = reinterpret_cast<LayerTestFn>(find(kModuleOn));
    const auto system = image.Rebase(kBattleSystem, sizeof(void*));
    const auto ui     = image.Rebase(facts::kEndUISystem, facts::kUIEnemyStatus + sizeof(void*));
    g_battleSystem    = system ? *system : 0;
    g_endUISystem     = ui ? *ui : 0;
    g_installed = g_setLock && facts::InBattleKnown() && g_release && g_cursorRelease && g_deactivate && g_moduleOn &&
                  system;
    if (!g_installed)
    {
        platform::Log(L"lock-on: UNAVAILABLE (a check failed); player 2 has no targeting of their own");
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
                             : L"player 2's targeting run UNAVAILABLE (no room to place its calls from the game "
                               L"thread)");
    }
    return placed;
}

void Remove() noexcept
{
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
    platform::Log(L"lock-on: {}", ok ? L"player 2's targeting is the game's own tick, run again in player 2's scopes (logged)"
                                     : L"player 2's targeting run UNAVAILABLE (a check failed); player 2 has no lock-on");
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

void AfterBattle() noexcept
{
    if (!g_installed || g_held || !generation::OnGameThread())
    {
        return;
    }
    if ((g_lock != 0 || g_displayed.controller != 0 || g_targetedKey.controller != 0 || g_cursor != 0 || g_freeTime != 0) &&
        !facts::InBattle())
    {
        EndOfBattle();
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"lock-on: player 2's targeting run {}", on ? L"on" : L"off");
}

void PlayerTwoOut() noexcept
{
    if (g_held)
    {
        return;
    }
    ReleaseCursor();
    ReleasePair(g_displayed);
    ReleasePair(g_targetedKey);
    g_lock     = 0;
    g_latch    = 0;
    g_freeTime = 0;
    g_trace    = 0;
}

bool Held() noexcept
{
    return g_held;
}

std::uintptr_t DisplayedObject() noexcept
{
    return g_displayed.object;
}

void Displayed(std::uintptr_t& object, std::uintptr_t& controller) noexcept
{
    object     = g_displayed.object;
    controller = g_displayed.controller;
}

void* Cursor() noexcept
{
    return reinterpret_cast<void*>(g_cursor);
}

bool TargetListOpen() noexcept
{
    return g_listOpen;
}

std::uint32_t TargetListOpenings() noexcept
{
    return g_listOpenings;
}

}  // namespace lss::player_two::lock_on
