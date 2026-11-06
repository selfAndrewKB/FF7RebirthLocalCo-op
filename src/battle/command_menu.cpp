#include "battle/command_menu.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "battle/command_locks.h"
#include "battle/command_menu_shared.h"
#include "battle/command_menu_tick.h"
#include "battle/focus_scope.h"
#include "battle/menu_rules.h"
#include "battle/shortcut_follow.h"
#include "facts/command_controller.h"
#include "facts/engine.h"
#include "facts/hud.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

// The controller's lifecycle (battle/command_menu.h): its build, the game's Setup and ResetAll made
// again on
// it, the Members panel rebuild and the squad switch followed, the join Setup, the placement and removal. The
// shortcut tables' follow is battle/shortcut_follow, the widgets per half battle/command_menu_view.

namespace lss::battle::command_menu {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// Facts read from the database (RE-p2-command-menu.md §1.1, §10; this build).
using facts::kMalloc;
constexpr std::uint8_t kCtorBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48,
                                       0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B,
                                       0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xBB, 0x64, 0x6A, 0xFE};
constexpr platform::FunctionFact kCtor{L"FieldCommandCtrl__Ctor", 0x143A08A54, kCtorBytes};
constexpr std::uint8_t kSetupBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55,
                                        0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                        0x8B, 0xEC, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x1A, 0x3E, 0xB2, 0x00};
constexpr platform::FunctionFact kSetup{L"FieldCommandCtrl__SetupMenuWidgets", 0x14158B0F4, kSetupBytes};
// The game's only calls of Setup and ResetAll on its controller (RE-p2-command-menu-levers.md §2): every
// path (field entry and exit steps, the fade-deferred Setup in MasterHUD_Tick, the Blueprint thunks, the
// menu-close re-Setup) ends at one of them. Both redirected; each call is made again on each seat's
// controller (Setup while the seat plays, ResetAll always). Each call's five bytes lie inside one aligned
// 16-byte block.
constexpr std::uintptr_t kSetupCall    = 0x140F52378;  // in SetupFieldCommandOrDefer 0x140F5231C
constexpr std::uintptr_t kResetAllCall = 0x140F52301;  // in ClearFieldCommandSetupPending 0x140F522DC
constexpr std::uintptr_t kResetAll     = 0x140F51D00;  // FieldCommandCtrl_ResetAll(ctrl)
// The Members panel's rebuild from the party (levers §16): FieldCommandCtrl_RefreshFieldMenu(ctrl) fills the
// party list's display array from the party manager's battle members. Setup calls it only when it creates
// the field menu; every later rebuild (field step 9 after a load, party changes, the party menu, scripted
// battles, the on/off table, Blueprint) goes through EndUI23_RefreshFieldMenu, which tail-jumps here with
// the game's controller. Its first instruction, `mov [rsp+8], rbx` (5 bytes, no RIP-relative operand, no
// branch lands inside it), is replaced; the rebuild also reads EndUISystem[23], so a seat's rebuild runs
// in that seat's focus scope.
constexpr std::uint8_t kRefreshBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00,
                                          0x00, 0xE8, 0xC0, 0x64, 0x4A, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9};
constexpr platform::FunctionFact kRefresh{L"FieldCommandCtrl_RefreshFieldMenu", 0x140C08A60, kRefreshBytes};
constexpr std::size_t            kRefreshFirst = 5;
// In 1.005 that first instruction crosses a 16-byte block, so the entry cannot be redirected there; its only two
// callers are redirected instead, each inside one block (RE-1005-battle-2.md): EndUI23_RefreshFieldMenu's tail jump
// and Setup's call (the same two as in this build).
constexpr std::uintptr_t kRefreshTailCall  = 0x140C08A59;  // jmp, in EndUI23_RefreshFieldMenu
constexpr std::uintptr_t kRefreshSetupCall = 0x142A77DF9;  // call, in Setup's cold chunk
// The in-field squad switch (levers §17): LB/RB in the field command menu (PartyList360_CycleSquad_ApplyAndShow)
// and the squad picked by index (PartyList360_SelectSquad_ApplyAndShow) apply the squad to the party, then
// lay out the Members panel of the controller that took the key directly, never through the rebuild above.
// Their one layout call each, LayoutFieldMenuCells(ctrl, 1, 0, 1), is redirected; after it, each seat's
// controller rebuilds its panel from the party (already changed). Each call inside one aligned 16-byte block.
constexpr std::uintptr_t kLayoutCells = 0x1408057E0;
constexpr std::array<std::uintptr_t, 2> kSquadLayoutCalls{0x143A49EDB, 0x143A49DC3};  // cycle (LB/RB), by index

constexpr std::size_t kControllerSize = 0x798;
// The key guide (EndUISystem[53]): the game builds its one with FMemory_Malloc(0x38) and KeyGuide53__Ctor
// (EndUISystem__Ctor); a seat's is built the same way (RE-p2-command-menu-views.md §10.4). Its widget
// (FWeakObjectPtr at +4) is created by the game's own ShowMode the first time the seat's controller drives it.
constexpr std::uint8_t kKeyGuideCtorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE0,
                                               0x0F, 0x69, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kKeyGuideCtor{L"KeyGuide53__Ctor", 0x143A1DF44, kKeyGuideCtorBytes};
constexpr std::size_t   kKeyGuideSize   = 0x38;
using facts::kUICommandController;  // the UI system's slots (facts/hud.h)
using facts::kUIKeyGuide;
// The controller's lifecycle facts (facts/command_controller.h; RE-p2-command-menu-lifecycle.md §2, §3.1).
using facts::kCmdSetupDone;
using facts::kCmdSetupPending;
using facts::kUISetupDeferred;
using facts::kCmdMenuLayer;
// What the log reads of a controller: the members (leader, ally, shortcut target, ally pick), the
// menu widget (FWeakObjectPtr) and its back pointer to the controller, and the focus bytes (§11 of
// RE-p2-command-menu-input.md: battle input, switching layer, +0x777, menu focus layer, ready, HUD mode).
using facts::kCmdMembers;
using facts::kCmdMenuWidget;
constexpr std::uint32_t kWidgetCtrl = 0x4E0;
using facts::kCmdLogBytes;
// UObjectBase: the class and the outer.
constexpr std::uint32_t kObjectClass = 0x10;
constexpr std::uint32_t kObjectOuter = 0x20;
// A refused join Setup (the seat's focus scope could not be entered) is a wait, never a give-up: the
// first kSetupTries tries run on consecutive ticks, later ones every kWaitGap ticks, until one runs
// (menu_rules::JoinTryDue).
using menu_rules::kSetupTries;

using MallocFn = void* (*)(std::size_t size, std::uint32_t alignment);
using CtorFn   = void* (*)(void* controller);
using SetupFn  = std::uint8_t (*)(void* controller, void* world);
using ResetAllFn = void (*)(void* controller);
using RefreshFn  = void (*)(void* controller);
using LayoutCellsFn = void (*)(void* controller, std::uint8_t all, std::uint32_t first, std::uint8_t force);
using KeyGuideCtorFn = void* (*)(void* keyGuide);

// Set once at install.
MallocFn  g_malloc  = nullptr;
CtorFn    g_ctor    = nullptr;
SetupFn   g_setup   = nullptr;
KeyGuideCtorFn g_keyGuideCtor = nullptr;
std::uintptr_t g_worldClass = 0;  // UWorld's class
platform::CallRedirect  g_setupCall, g_resetAllCall;
platform::EntryRedirect g_refresh;
platform::CallRedirect  g_refreshTailCall, g_refreshSetupCall;  // 1.005: the rebuild's two callers instead
RefreshFn               g_refreshFn = nullptr;                   // 1.005: the rebuild itself (set before its calls)
std::array<platform::CallRedirect, kSquadLayoutCalls.size()> g_squadLayoutCalls;
LayoutCellsFn g_layoutCells = nullptr;  // the calls' target, set before either is redirected
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t resetAt = 0, resetAll = 0, setupAt = 0, refresh = 0;
};
Sites                              g_sites;
platform::DeferredSites g_placement;  // 1.005: the calls placed from the game thread (platform/deferred_sites.h)
constexpr const wchar_t* kUnavailable = L"UNAVAILABLE (a check failed)";

struct Snapshot
{
    std::array<std::int32_t, 4>          members{};
    std::array<std::uint8_t, kCmdLogBytes.size()> bytes{};
    void*                                widget = nullptr;
    bool                                 widgetNamesUs = false;
    focus_scope::Summary                 context{};
    bool operator==(const Snapshot& other) const
    {
        return members == other.members && bytes == other.bytes && widget == other.widget &&
               widgetNamesUs == other.widgetNamesUs && context.pathCount == other.context.pathCount &&
               context.layerCount == other.context.layerCount;
    }
};

// Game thread. A seat's controller outlives the mod's stages, like the game's.
struct SeatMenu
{
    void* controller = nullptr;
    void* keyGuide   = nullptr;  // the seat's key guide (built after the controller; kept like it)
    bool  failed     = false;    // a game call faulted on this controller: it follows nothing more
    // The seat's controller is set up for the game's current setup: its last Setup followed the game's (or
    // was the join Setup after it). The game's ResetAll clears it; so does a Setup of the game's that could
    // not be made again on the seat's (the seat out, or a scope refused), and the join Setup then runs.
    bool     current   = false;
    int      tries     = 0;  // join Setup tries since `current` was last cleared, refusals included
    unsigned waitTicks = 0;  // ticks counted between the paced tries
    Snapshot logged{};
    bool     loggedOnce = false;
    std::array<std::uint8_t, 3> loggedLifecycle{0xFF, 0xFF, 0xFF};  // P1 setup done, deferred, the seat's setup done
};
std::array<SeatMenu, players::kMaxPlayers> g_seats{};  // by seat; index 0 unused (player 1's is the game's own)
bool           g_installed   = false;
std::uintptr_t g_endUISystem = 0;

/// `seat`'s record. Every caller has checked the seat; a bad one (a bug) gets a scratch record, cleared each time, and
/// one warning, so no seat's record is touched.
SeatMenu& SeatOf(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        return g_seats[static_cast<std::size_t>(seat)];
    }
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        platform::Log(L"command menu: WARNING: a record was asked for player index {} (player 1 is 0), which does not exist; a scratch "
                      L"record answered",
                      seat);
    }
    static SeatMenu scratch;
    scratch = {};
    return scratch;
}

const wchar_t* Name(int seat) noexcept
{
    return players::SeatName(seat);
}

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"command menu: {}", text);
}

/// The world player 1's controller is in (the first outer of class UWorld; guarded reads), or null.
void* WorldOf(void* controller) noexcept
{
    auto object = reinterpret_cast<std::uintptr_t>(controller);
    for (int depth = 0; object != 0 && g_worldClass != 0 && depth < 4; ++depth)
    {
        object = ReadAt<std::uintptr_t>(object + kObjectOuter);
        if (object != 0 && ReadAt<std::uintptr_t>(object + kObjectClass) == g_worldClass)
        {
            return reinterpret_cast<void*>(object);
        }
    }
    return nullptr;
}

/// How many command menu widgets exist (each controller's Setup creates one when it has none).
std::size_t CountMenus() noexcept
{
    try
    {
        std::vector<RC::Unreal::UObject*> menus;
        RC::Unreal::UObjectGlobals::FindAllOf(STR("Command_C"), menus);
        return menus.size();
    }
    catch (...)
    {
        return 0;
    }
}

enum class SetupResult
{
    Ran,
    Refused,
    Faulted
};

/// Runs `call` (a guarded game call on `seat`'s controller, true when it did not fault) in the seat's focus
/// context (the UI system's controller and key guide slots name the seat's) and, when the seat's member is
/// known, with the party leader answering that member (Setup stores the leader's type in the controller).
template <class Call>
SetupResult InSeatScopes(int seat, const wchar_t*& refusal, Call&& call) noexcept
{
    void*          member = players::generation::MemberOf(seat);
    seat_ladder::Args args;
    args.seat       = seat;
    args.controller = SeatOf(seat).controller;
    // The leader pair when the member is known, and not required (the call runs without it, as before).
    const unsigned leader = member != nullptr && seat_ladder::FillMember(args, member)
                                ? seat_ladder::Leader | seat_ladder::LeaderType
                                : 0u;
    seat_ladder::SeatScopes scopes(seat_ladder::Focus | leader, leader, args);
    if (!scopes.Held())
    {
        refusal = scopes.Refusal();
        return SetupResult::Refused;
    }
    if (!call())
    {
        scopes.Get<seat_ladder::FocusPart>()->scope.Abandon();
        return SetupResult::Faulted;
    }
    return SetupResult::Ran;
}

SetupResult RunSetup(int seat, void* world, std::uint8_t& result, const wchar_t*& refusal) noexcept
{
    void* const controller = SeatOf(seat).controller;
    return InSeatScopes(seat, refusal, [&] { return platform::CallGuarded(g_setup, result, controller, world); });
}

/// Logs the "setup done" bytes of player 1's and `seat`'s controllers and the deferred mark on change.
void WatchLifecycle(int seat) noexcept
{
    SeatMenu&  mine = SeatOf(seat);
    const auto one  = ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController);
    const auto own  = reinterpret_cast<std::uintptr_t>(mine.controller);
    const std::array<std::uint8_t, 3> lifecycle{ReadAt<std::uint8_t>(one + kCmdSetupDone),
                                                ReadAt<std::uint8_t>(g_endUISystem + kUISetupDeferred),
                                                own != 0 ? ReadAt<std::uint8_t>(own + kCmdSetupDone)
                                                         : std::uint8_t{0xFF}};
    if (lifecycle != mine.loggedLifecycle)
    {
        mine.loggedLifecycle = lifecycle;
        Line(std::format(L"setup done: player 1 {} (a setup deferred: {}), {} {} (follows the game's: {})",
                         lifecycle[0], lifecycle[1], Name(seat), lifecycle[2], players::YesNo(mine.current)));
    }
}

void ReplayLine(const std::wstring& text) noexcept
{
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"command menu: {}", text);
    }
}

}  // namespace

int SeatOfController(const void* controller) noexcept
{
    for (int seat = 1; controller != nullptr && seat < players::kMaxPlayers; ++seat)
    {
        if (g_seats[static_cast<std::size_t>(seat)].controller == controller)
        {
            return seat;
        }
    }
    return -1;
}

/// The game's call may be made again on `seat`'s controller: it is built, we are on the game thread, the call was
/// made on the game's controller (the UI system's slot names it, and it is no seat's), and, when the call needs the
/// seat's member (Setup stores the leader's type), the seat plays.
bool CanFollow(int seat, void* gameController, bool needsPlayer) noexcept
{
    if (!players::SeatIndex(seat))
    {
        return false;
    }
    const SeatMenu& mine = SeatOf(seat);
    return menu_rules::CanFollow(g_installed, mine.failed, mine.controller != nullptr, players::generation::OnGameThread(),
                                 needsPlayer, players::generation::Active(seat), gameController != nullptr,
                                 SeatOfController(gameController) >= 1,
                                 reinterpret_cast<std::uintptr_t>(gameController) ==
                                     ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController));
}

bool Ready(int seat) noexcept
{
    return g_installed && players::SeatIndex(seat) && !SeatOf(seat).failed;
}

std::uintptr_t UiSystem() noexcept
{
    return g_endUISystem;
}

namespace {

/// The game's Members panel rebuild, for the mod's calls: the entry redirect's trampoline, or on 1.005 the function
/// itself (its two calls are redirected instead).
RefreshFn RefreshFunction() noexcept
{
    return g_refreshFn != nullptr ? g_refreshFn : reinterpret_cast<RefreshFn>(g_refresh.Original());
}

/// The Members panel of `seat`'s controller rebuilt from the party, in the seat's scopes.
void RefreshSeat(int seat, const wchar_t* why) noexcept
{
    const auto     refresh    = RefreshFunction();
    void* const    controller = SeatOf(seat).controller;
    const wchar_t* refusal    = nullptr;
    switch (InSeatScopes(seat, refusal, [&] { return platform::CallGuarded(refresh, controller); }))
    {
        case SetupResult::Ran:
            ReplayLine(std::format(L"{}'s Members panel rebuilt from the party ({})", Name(seat), why));
            break;
        case SetupResult::Refused:
            ReplayLine(std::format(L"{}'s Members panel NOT rebuilt ({}): {}", Name(seat), why, refusal));
            break;
        case SetupResult::Faulted:
            Line(std::format(L"FAILED: rebuilding {}'s Members panel faulted ({})", Name(seat), why));
            break;
    }
}

/// Replaces the Members panel's rebuild: the game's controller's, then each seat's in seat order (a rebuild of a
/// seat's own, from its Setup or from us, runs once).
void RefreshCall(void* controller) noexcept
{
    RefreshFunction()(controller);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (CanFollow(seat, controller, false))
        {
            RefreshSeat(seat, L"the game rebuilt its own");
        }
    }
}

/// Replaces the squad switch's layout call: the game lays out its own panel, then each seat's is rebuilt
/// from the party the switch just changed. (The switch is a field-menu key; a seat's field presses go to
/// player 1's menu, so the controller here is the game's.)
void SquadLayoutCall(void* controller, std::uint8_t all, std::uint32_t first, std::uint8_t force) noexcept
{
    g_layoutCells(controller, all, first, force);
    if (const int owner = SeatOfController(controller); owner >= 1)
    {
        Line(std::format(L"WARNING: the in-field squad switch ran on {}'s controller; the game's panel is not rebuilt",
                         Name(owner)));
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (CanFollow(seat, controller, false))
        {
            RefreshSeat(seat, L"the game's in-field squad switch");
        }
    }
}

/// The game's Setup made again on `seat`'s controller with the same world, in the seat's focus context and with
/// the leader answering the seat's member.
void FollowSetup(int seat, void* controller, void* world) noexcept
{
    SeatMenu& mine = SeatOf(seat);
    mine.current   = false;
    mine.tries     = 0;
    if (!CanFollow(seat, controller, true))
    {
        ReplayLine(std::format(L"the game set up its controller; {}'s follows when {} plays (join Setup)", Name(seat),
                               Name(seat)));
        return;
    }
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    switch (RunSetup(seat, world, set, refusal))
    {
        case SetupResult::Ran:
        {
            mine.current   = true;
            const auto own = reinterpret_cast<std::uintptr_t>(mine.controller);
            ReplayLine(std::format(L"the game's Setup made on {}'s controller too (returned {}): 777={} 783={}",
                                   Name(seat), set, ReadAt<std::uint8_t>(own + kCmdSetupDone),
                                   ReadAt<std::uint8_t>(own + kCmdMenuLayer)));
            break;
        }
        case SetupResult::Refused:
            ReplayLine(std::format(L"the game's Setup NOT made on {}'s controller: {}; the join Setup follows",
                                   Name(seat), refusal));
            break;
        case SetupResult::Faulted:
            mine.failed = true;
            Line(std::format(L"FAILED: the game's Setup faulted on {}'s controller; {}'s menu, attack, guard and dodge "
                             L"stay off",
                             Name(seat), Name(seat)));
            break;
    }
}

/// Replaces the game's Setup call: the game's controller, then each seat's in seat order.
std::uint8_t SetupCall(void* controller, void* world) noexcept
{
    const std::uint8_t answer = reinterpret_cast<SetupFn>(g_setupCall.Original())(controller, world);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        FollowSetup(seat, controller, world);
    }
    return answer;
}

/// The game's ResetAll made again on `seat`'s controller, in the seat's focus context (ResetAll also resets the key
/// guide the scope names, the seat's) and, when the seat plays, with the leader answering its member. ResetAll needs
/// no member, so the seat's follows it even while the seat is out (the field exit takes the seats out first). The
/// seat's then waits for the game's next Setup, or the join Setup, as the game's waits for its own.
void FollowResetAll(int seat, void* controller) noexcept
{
    SeatMenu& mine = SeatOf(seat);
    mine.current   = false;
    mine.tries     = 0;
    if (!CanFollow(seat, controller, false))
    {
        ReplayLine(std::format(L"the game reset its controller; {}'s is not built yet", Name(seat)));
        return;
    }
    const auto     resetAll = reinterpret_cast<ResetAllFn>(g_resetAllCall.Original());
    void* const    own      = mine.controller;
    const wchar_t* refusal  = nullptr;
    switch (InSeatScopes(seat, refusal, [&] { return platform::CallGuarded(resetAll, own); }))
    {
        case SetupResult::Ran:
            ReplayLine(std::format(L"the game's ResetAll made on {}'s controller too: 777={}", Name(seat),
                                   ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(own) + kCmdSetupDone)));
            break;
        case SetupResult::Refused:
            ReplayLine(std::format(L"the game's ResetAll NOT made on {}'s controller: {}", Name(seat), refusal));
            break;
        case SetupResult::Faulted:
            mine.failed = true;
            Line(std::format(L"FAILED: the game's ResetAll faulted on {}'s controller; {}'s menu, attack, guard and "
                             L"dodge stay off",
                             Name(seat), Name(seat)));
            break;
    }
}

/// Replaces the game's ResetAll call: the game's controller, then each seat's in seat order.
void ResetAllCall(void* controller) noexcept
{
    reinterpret_cast<ResetAllFn>(g_resetAllCall.Original())(controller);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        FollowResetAll(seat, controller);
    }
}

/// Allocates and constructs `seat`'s controller, as EndUISystem__Ctor does for the game's.
bool Build(int seat) noexcept
{
    SeatMenu& mine = SeatOf(seat);
    void* memory = nullptr;
    if (!platform::CallGuarded(g_malloc, memory, kControllerSize, std::uint32_t{0}) || memory == nullptr)
    {
        return false;
    }
    std::memset(memory, 0, kControllerSize);
    void* built = nullptr;
    if (!platform::CallGuarded(g_ctor, built, memory))
    {
        return false;  // the memory is left: the ctor may have registered it before faulting
    }
    mine.controller = memory;
    Line(std::format(L"built {}'s controller at {:#x}", Name(seat), reinterpret_cast<std::uintptr_t>(memory)));
    // The seat's key guide, as EndUISystem__Ctor builds the game's. Optional: without it the seat's controller
    // leaves the shared one alone (battle/command_menu_tick skips its key-guide writes).
    void* guide = nullptr;
    if (platform::CallGuarded(g_malloc, guide, kKeyGuideSize, std::uint32_t{0}) && guide != nullptr)
    {
        std::memset(guide, 0, kKeyGuideSize);
        void* built2 = nullptr;
        if (platform::CallGuarded(g_keyGuideCtor, built2, guide))
        {
            mine.keyGuide = guide;
            focus_scope::SetKeyGuide(seat, guide);
        }
    }
    Line(std::format(L"{}'s key guide {}", Name(seat),
                     mine.keyGuide != nullptr ? L"built" : L"NOT built (the shared one stays)"));
    return true;
}

Snapshot Read(int seat) noexcept
{
    Snapshot   now;
    const auto ctrl = reinterpret_cast<std::uintptr_t>(SeatOf(seat).controller);
    for (std::size_t i = 0; i < now.members.size(); ++i)
    {
        now.members[i] = ReadAt<std::int32_t>(ctrl + kCmdMembers + i * 4);
    }
    for (std::size_t i = 0; i < kCmdLogBytes.size(); ++i)
    {
        now.bytes[i] = ReadAt<std::uint8_t>(ctrl + kCmdLogBytes[i]);
    }
    now.widget = players::weak_object::ObjectOf(ReadAt<std::uint64_t>(ctrl + kCmdMenuWidget));
    now.widgetNamesUs = now.widget != nullptr &&
                        ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(now.widget) + kWidgetCtrl) == ctrl;
    now.context = focus_scope::ContextOf(seat);
    return now;
}

void Watch(int seat) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    SeatMenu&      mine = SeatOf(seat);
    const Snapshot now  = Read(seat);
    if (mine.loggedOnce && now == mine.logged)
    {
        return;
    }
    mine.logged     = now;
    mine.loggedOnce = true;
    const auto& b = now.bytes;
    platform::LogDebug(L"command menu: {}", std::format(L"members {} {} {} {} | menu {} (names this controller: {}) | 25C={} 258={} 777={} 783={} 5E1={} "
                     L"540={} | {}'s focus: path {}, layers {}",
                     now.members[0], now.members[1], now.members[2], now.members[3], players::NameOf(now.widget),
                     players::YesNo(now.widgetNamesUs), b[0], b[1], b[2], b[3], b[4], b[5], Name(seat), now.context.pathCount,
                     now.context.layerCount));
}

/// One call site redirected: at install on the research build, from the game thread on 1.005.
template <class Fn>
bool Place(platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, Fn* with) noexcept
{
    return g_placement.Place(call, site, target, reinterpret_cast<std::uintptr_t>(with));
}

/// The Members panel rebuild followed at every caller: its entry redirected, or on 1.005 (where that entry crosses a
/// block) its two calls.
bool InstallRefresh(const platform::GameImage& image, std::uintptr_t refresh) noexcept
{
    if (platform::RunningBuild() != platform::BuildIndex::V1005)
    {
        return g_refresh.Install(refresh, image.EntryInstruction(kRefresh.address, std::span<const std::uint8_t>(kRefreshBytes, kRefreshFirst)),
                                 reinterpret_cast<std::uintptr_t>(&RefreshCall));
    }
    const auto tail = image.Rebase(kRefreshTailCall, 5), call = image.Rebase(kRefreshSetupCall, 5);
    g_refreshFn     = reinterpret_cast<RefreshFn>(refresh);
    return tail && call && g_refreshTailCall.InstallTailJump(*tail, refresh, reinterpret_cast<std::uintptr_t>(&RefreshCall)) &&
           Place(g_refreshSetupCall, *call, refresh, &RefreshCall);
}

/// The redirects, from what Install found (its checks passed). At install on the research build; on 1.005 from
/// the game thread.
bool PlaceSites(const platform::GameImage& image) noexcept
{
    const Sites& s = g_sites;
    // ResetAll first: a Setup followed without its ResetAll is the join Setup's case anyway.
    g_installed = Place(g_resetAllCall, s.resetAt, s.resetAll, &ResetAllCall) &&
                  Place(g_setupCall, s.setupAt, reinterpret_cast<std::uintptr_t>(g_setup), &SetupCall) &&
                  InstallRefresh(image, s.refresh);
    const auto layoutCells = image.Rebase(kLayoutCells);
    g_layoutCells          = layoutCells ? reinterpret_cast<LayoutCellsFn>(*layoutCells) : nullptr;
    for (std::size_t i = 0; g_installed && i < kSquadLayoutCalls.size(); ++i)
    {
        const auto at = image.Rebase(kSquadLayoutCalls[i], 5);
        g_installed   = layoutCells && at && Place(g_squadLayoutCalls[i], *at, *layoutCells, &SquadLayoutCall);
    }
    if (!g_installed)
    {
        Remove();
    }
    else
    {
        shortcut_follow::Place(image, g_placement);
    }
    platform::Log(L"command menu: {}", g_installed ? L"player 2's and 3's command controllers are each built once they "
                                                         L"play and follow the game's Setup, ResetAll and Members panel "
                                                         L"rebuild (logged)"
                                                       : kUnavailable);
    return g_installed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto malloc = platform::FindFunction(image, kMalloc);
    const auto ctor   = platform::FindFunction(image, kCtor);
    const auto setup  = platform::FindFunction(image, kSetup);
    const auto guide  = platform::FindFunction(image, kKeyGuideCtor);
    g_keyGuideCtor    = guide ? reinterpret_cast<KeyGuideCtorFn>(*guide) : nullptr;
    try
    {
        g_worldClass = reinterpret_cast<std::uintptr_t>(
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, STR("/Script/Engine.World")));
    }
    catch (...)
    {
        g_worldClass = 0;
    }
    const auto system = image.Rebase(facts::kEndUISystem, kUIKeyGuide + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    const auto site   = [&image](std::uintptr_t address) { return image.Rebase(address, 5); };
    const auto resetAll = image.Rebase(kResetAll);
    const auto refresh  = platform::FindFunction(image, kRefresh);
    const auto setupAt = site(kSetupCall), resetAt = site(kResetAllCall);
    g_installed = malloc && ctor && setup && players::weak_object::Known() && g_worldClass != 0 && system && resetAll &&
                  refresh && setupAt && resetAt && focus_scope::Install(image);
    for (SeatMenu& seat : g_seats)
    {
        seat.current = false;
        seat.tries   = 0;
    }
    if (g_installed)
    {
        g_malloc  = reinterpret_cast<MallocFn>(*malloc);
        g_ctor    = reinterpret_cast<CtorFn>(*ctor);
        g_setup   = reinterpret_cast<SetupFn>(*setup);
        g_sites   = {*resetAt, *resetAll, *setupAt, *refresh};
        shortcut_follow::Find(image);
    }
    if (!g_installed)
    {
        platform::Log(L"command menu: {}", kUnavailable);
        return false;
    }
    // A squad layout call crosses a 16-byte block in 1.005, and the rebuild's entry too: every site is placed from
    // the game thread, between its ticks (platform/deferred_sites.h).
    if (platform::DeferredSites::Deferred())
    {
        g_installed = false;  // until the placement: the controller is not built before its calls are in
    }
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"command menu: {}",
                      placed ? L"its calls are placed from the game thread (1.005: two cross a 16-byte block)"
                             : L"UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_installed = false;  // the controller stays: the game's GC list refers to it
    g_placement.Cancel();  // a placement still queued does nothing
    if (g_refresh.IsInstalled() && !g_refresh.Restore())
    {
        platform::Log(L"command menu: the Members panel rebuild was changed after us; left in place (it passes through)");
    }
    shortcut_follow::Remove();
    for (auto* call : {&g_squadLayoutCalls[0], &g_squadLayoutCalls[1], &g_setupCall, &g_resetAllCall, &g_refreshTailCall,
                       &g_refreshSetupCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu: a call was changed after us; left in place (it passes through)");
        }
    }
}

void* Controller(int seat) noexcept
{
    return players::SeatIndex(seat) ? SeatOf(seat).controller : nullptr;
}

void* KeyGuide(int seat) noexcept
{
    return players::SeatIndex(seat) ? SeatOf(seat).keyGuide : nullptr;
}

namespace {

/// After the join Setup: a seat's command menu gets its UI focus again through the game's own calls, in the
/// seat's scopes, when the game's controller has battle input on. The game's
/// ResumeHudInput(0) as its field starts keeps battle input on, with its menu's focus layer up, through the
/// field and every battle. A seat missed it while out, and its controller outlives the leave with its
/// flag and layer byte (+0x783) still set, while the layer itself went with the focus of the player who
/// left. The battle-input mirror follows only a change, and the game's layer setter returns early while
/// the byte is set, so the seat's focus would stay off its menu: its pad's Attack, Unique and Guard would
/// reach nothing (the user's retry tests, v0.28.0). The layer setter's off call, then ResumeHudInput(0)
/// (command_menu_tick::RefocusMenu). A written exception, as the lock sync: missed game calls made once at
/// the join.
void RefocusAtJoin(int seat, std::uintptr_t one) noexcept
{
    if (!command_menu_tick::BattleInputOn(reinterpret_cast<const void*>(one)))
    {
        return;
    }
    void* const    own     = SeatOf(seat).controller;
    const auto     layer   = ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(own) + kCmdMenuLayer);
    const wchar_t* refusal = nullptr;
    switch (InSeatScopes(seat, refusal, [own] { return command_menu_tick::RefocusMenu(own); }))
    {
        case SetupResult::Ran:
            Line(std::format(L"join: {}'s menu focus layer {}put up again through the game's calls (layer byte "
                             L"was {}); battle input on",
                             Name(seat), layer != 0 ? L"taken down and " : L"", layer));
            break;
        case SetupResult::Refused:
            Line(std::format(L"join: {}'s menu focus NOT restored: {}", Name(seat), refusal));
            break;
        case SetupResult::Faulted:
            Line(std::format(L"join: restoring {}'s menu focus FAULTED", Name(seat)));
            break;
    }
}

/// One seat's tick: builds its controller, runs the join Setup when it does not follow the game's current setup,
/// and logs its bytes on change.
void SeatTick(int seat) noexcept
{
    SeatMenu& mine = SeatOf(seat);
    if (mine.failed || !players::generation::Active(seat))
    {
        return;
    }
    WatchLifecycle(seat);
    shortcut_follow::NoteGameController();  // kept for a shortcut rebuild that runs in a seat's scope
    if (mine.current)
    {
        Watch(seat);
        return;
    }
    // The join Setup: the seat's controller is built or set up when the seat plays and it does not follow
    // the game's current setup (the seat joined, or was out or refused at the game's Setup). It follows the
    // game's controller: only once that one is set up and no Setup of its own is pending or deferred.
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kUICommandController);
    if (one == 0 || !menu_rules::GameSetUp(one, ReadAt<std::uint8_t>(one + kCmdSetupDone),
                                           ReadAt<std::uint8_t>(one + kCmdSetupPending),
                                           ReadAt<std::uint8_t>(g_endUISystem + kUISetupDeferred)))
    {
        return;
    }
    void* world = WorldOf(players::generation::PlayerOneController());
    if (world == nullptr)
    {
        return;
    }
    if (!menu_rules::JoinTryDue(mine.tries, mine.waitTicks))
    {
        return;  // refused so far: paced tries, never given up
    }
    if (mine.controller == nullptr && !Build(seat))
    {
        mine.failed = true;
        Line(std::format(L"FAILED to build {}'s controller; {}'s menu, attack, guard and dodge stay off", Name(seat),
                         Name(seat)));
        return;
    }
    ++mine.tries;
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    const auto     setup   = RunSetup(seat, world, set, refusal);
    if (setup == SetupResult::Refused)
    {
        if (mine.tries <= kSetupTries || mine.tries % 20 == 0)
        {
            Line(std::format(L"join setup waits: {} (try {})", refusal, mine.tries));
        }
        return;
    }
    if (setup == SetupResult::Faulted)
    {
        mine.failed = true;
        Line(std::format(L"FAILED: the join setup faulted; {}'s menu, attack, guard and dodge stay off", Name(seat)));
        return;
    }
    mine.current    = true;
    mine.tries      = 0;
    mine.loggedOnce = false;
    // After the game's Setup, field step 9 rebuilds the game's Members panel from the party; the seat's missed it
    // (the seat was out), so it runs now, in the game's order.
    RefreshSeat(seat, L"after the join Setup, as field step 9 follows the game's");
    command_locks::Sync(seat, mine.controller);
    RefocusAtJoin(seat, one);
    // Only at a join (an object walk is slow): a second Setup must not leave extra menus behind.
    Line(std::format(L"join setup for world '{}' (returned {}); command menu widgets now: {}", players::NameOf(world), set,
                     CountMenus()));
    Watch(seat);
}

}  // namespace

void Tick() noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        SeatTick(seat);
    }
}

}  // namespace lss::battle::command_menu
