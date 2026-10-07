#include "player_two/command_menu_two.h"

#include <atomic>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "hud/widget_opacity.h"
#include "player_two/command_locks.h"
#include "player_two/command_menu_tick.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/focus_scope.h"
#include "player_two/game_thread_sites.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/party_member.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lss::player_two::command_menu_two {

namespace {

// Facts read from the database (RE-p2-command-menu.md §1.1, §10; this build).
constexpr std::uint8_t kMallocBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                         0x00, 0x00, 0x00, 0xE8, 0x48, 0xFB, 0x86, 0x01};
constexpr platform::FunctionFact kMalloc{L"FMemory_Malloc", 0x14083F3D8, kMallocBytes};
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
// menu-close re-Setup) ends at one of them. Both redirected; each call is made again on player 2's
// controller (Setup while player 2 plays, ResetAll always). Each call's five bytes lie inside one aligned
// 16-byte block.
constexpr std::uintptr_t kSetupCall    = 0x140F52378;  // in SetupFieldCommandOrDefer 0x140F5231C
constexpr std::uintptr_t kResetAllCall = 0x140F52301;  // in ClearFieldCommandSetupPending 0x140F522DC
constexpr std::uintptr_t kResetAll     = 0x140F51D00;  // FieldCommandCtrl_ResetAll(ctrl)
// The Members panel's rebuild from the party (levers §16): FieldCommandCtrl_RefreshFieldMenu(ctrl) fills the
// party list's display array from the party manager's battle members. Setup calls it only when it creates
// the field menu; every later rebuild (field step 9 after a load, party changes, the party menu, scripted
// battles, the on/off table, Blueprint) goes through EndUI23_RefreshFieldMenu, which tail-jumps here with
// the game's controller. Its first instruction, `mov [rsp+8], rbx` (5 bytes, no RIP-relative operand, no
// branch lands inside it), is replaced; the rebuild also reads EndUISystem[23], so player 2's runs in
// player 2's focus scope.
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
// Their one layout call each, LayoutFieldMenuCells(ctrl, 1, 0, 1), is redirected; after it, player 2's
// controller rebuilds its panel from the party (already changed). Each call inside one aligned 16-byte block.
constexpr std::uintptr_t kLayoutCells = 0x1408057E0;
constexpr std::array<std::uintptr_t, 2> kSquadLayoutCalls{0x143A49EDB, 0x143A49DC3};  // cycle (LB/RB), by index

constexpr std::size_t kControllerSize = 0x798;
// The key guide (EndUISystem[53]): the game builds its one with FMemory_Malloc(0x38) and KeyGuide53__Ctor
// (EndUISystem__Ctor); player 2's is built the same way (RE-p2-command-menu-views.md §10.4). Its widget
// (FWeakObjectPtr at +4) is created by the game's own ShowMode the first time player 2's controller drives it.
constexpr std::uint8_t kKeyGuideCtorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE0,
                                               0x0F, 0x69, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kKeyGuideCtor{L"KeyGuide53__Ctor", 0x143A1DF44, kKeyGuideCtorBytes};
constexpr std::size_t   kKeyGuideSize   = 0x38;
constexpr std::uint32_t kKeyGuideSlot   = 0x1A8;  // EndUISystem[53]
constexpr std::uint32_t kKeyGuideWidget = 0x04;
// The UI system's command controller slot (EndUISystem[23]: the game's controller).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kGameController = 0xB8;
constexpr std::uint32_t  kHudMode        = 0x540;  // u8: the battle HUD mode
// The controller's lifecycle (RE-p2-command-menu-lifecycle.md §2, §3.1): "setup done" (+0x777: Setup sets
// it, only ResetAll clears it; while 0 no menu state can be pushed) and "setup pending" (+0x775). The
// UI system's +0x2D1 marks a Setup deferred behind a fade.
constexpr std::uint32_t kSetupDone     = 0x777;
constexpr std::uint32_t kSetupPending  = 0x775;
constexpr platform::PerBuild<std::uint32_t> kSetupDeferred = {0x2D1, 0x2D9};  // in the UI system; 1.005: EndUISystem__ClearFieldCommandSetupPending 0x1413FBB7A / SetupFieldCommandOrDefer 0x1413FBBE2 mov byte [..+2D9h],0 (EndUISystem +8 from between +0x2C0 and +0x2D0)
constexpr std::uint32_t kSelecterSet   = 0x776;  // u8, set by Setup (logged)
constexpr std::uint32_t kSetupOther    = 0x778;  // u8, set by Setup (logged)
constexpr std::uint32_t kMenuLayer     = 0x783;  // u8 (logged)
constexpr int           kWaitGap       = 30;     // engine ticks between paced join tries
// What the log reads of a controller: the members (leader, ally, shortcut target, ally pick), the
// menu widget (FWeakObjectPtr) and its back pointer to the controller, and the focus bytes (§11 of
// RE-p2-command-menu-input.md: battle input, switching layer, +0x777, menu focus layer, ready, HUD mode).
constexpr std::uint32_t kMembers    = 0x34;
constexpr std::uint32_t kMenuWidget = 0x4F0;
constexpr std::uint32_t kWidgetCtrl = 0x4E0;
constexpr std::array<std::uint32_t, 6> kBytes{0x25C, 0x258, 0x777, 0x783, 0x5E1, 0x540};
// UObjectBase: the class and the outer.
constexpr std::uint32_t kObjectClass = 0x10;
constexpr std::uint32_t kObjectOuter = 0x20;
// A refused join Setup (player 2's focus scope could not be entered) is a wait, never a give-up: the
// first kSetupTries tries run on consecutive ticks, later ones every kWaitGap ticks, until one runs.
constexpr int kSetupTries = 5;
constexpr int kMaxLines   = 200;
constexpr int kMaxReplayLines = 60;  // the game's Setup and ResetAll made again on player 2's controller

using MallocFn = void* (*)(std::size_t size, std::uint32_t alignment);
using CtorFn   = void* (*)(void* controller);
using SetupFn  = std::uint8_t (*)(void* controller, void* world);
using ResetAllFn = void (*)(void* controller);
using RefreshFn  = void (*)(void* controller);
using LayoutCellsFn = void (*)(void* controller, std::uint8_t all, std::uint32_t first, std::uint8_t force);
using KeyGuideCtorFn = void* (*)(void* keyGuide);
using WeakGetFn = void* (*)(const std::uint64_t* weak);

// Set once at install.
MallocFn  g_malloc  = nullptr;
CtorFn    g_ctor    = nullptr;
SetupFn   g_setup   = nullptr;
KeyGuideCtorFn g_keyGuideCtor = nullptr;
WeakGetFn g_weakGet = nullptr;
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
std::optional<platform::GameImage> g_placeImage;           // 1.005: kept for the placement
std::atomic<bool>                   g_fromGameThread {false};  // 1.005: the calls go in with InstallFromOwnThread

// Game thread. The controller outlives the mod's stages, like the game's.
void*         g_controller = nullptr;
void*         g_keyGuide   = nullptr;  // player 2's key guide (built after the controller; kept like it)
bool          g_installed  = false;
bool          g_failed     = false;
// Player 2's controller is set up for the game's current setup: its last Setup followed the game's (or
// was the join Setup after it). The game's ResetAll clears it; so does a Setup of the game's that could
// not be made again on player 2's (player 2 out, or a scope refused), and the join Setup then runs.
bool          g_current    = false;
int           g_tries      = 0;        // join Setup tries since g_current was last cleared, refusals included
unsigned      g_waitTicks  = 0;        // ticks counted between the paced tries
int           g_lines      = 0;
int           g_replayLines = 0;

struct Snapshot
{
    std::array<std::int32_t, 4>          members{};
    std::array<std::uint8_t, kBytes.size()> bytes{};
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
Snapshot g_logged{};
bool     g_loggedOnce = false;
int      g_hides      = 0;  // times the menu widget was found shown and hidden again
std::array<std::uint8_t, 3> g_loggedLifecycle{0xFF, 0xFF, 0xFF};  // P1 setup done, deferred, P2 setup done
std::uintptr_t g_endUISystem = 0;
// A controller's widgets shown per half (RE-p2-command-menu-views.md §10.3, §10.5): the command
// menu, the field menu (the party list) and the wait-mode filter (the tactical-mode tint), each an
// FWeakObjectPtr in the controller. In player 2's pass, player 2's command and field menus take player
// 1's own opacity (their fades), the filter its full opacity (its own content decides).
struct Part
{
    std::uint32_t offset;
    bool          borrowOpacity;
};
constexpr std::array<Part, 3> kParts{{{kMenuWidget, true}, {0x4E8, true}, {0x62C, false}}};
// Render (game thread): player 1's widgets (FWeakObjectPtr, resolved again at each use: the game can
// rebuild them) and their own opacity while player 2's pass shows player 2's.
struct Saved
{
    std::uint64_t weak    = 0;
    float         opacity = 1.0F;
};
std::array<Saved, kParts.size() + 1> g_oneSaved{};  // the parts, then the key guide's widget
bool                             g_swapped = false;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    if (address != 0)
    {
        platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }
    return value;
}

void Line(const std::wstring& text) noexcept
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"command menu two: {}", text);
    }
}

/// The world player 1's controller is in (the first outer of class UWorld; guarded reads), or null.
void* WorldOf(void* controller) noexcept
{
    auto object = reinterpret_cast<std::uintptr_t>(controller);
    for (int depth = 0; object != 0 && g_worldClass != 0 && depth < 4; ++depth)
    {
        object = Field<std::uintptr_t>(object + kObjectOuter);
        if (object != 0 && Field<std::uintptr_t>(object + kObjectClass) == g_worldClass)
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

/// Runs `call` (a guarded game call on player 2's controller, true when it did not fault) in player 2's
/// focus context (the UI system's controller and key guide slots name player 2's) and, when player 2's
/// member is known, with the party leader answering player 2's member (Setup stores the leader's type in
/// the controller).
template <class Call>
SetupResult InPlayerTwoScopes(const wchar_t*& refusal, Call&& call) noexcept
{
    void*      member     = generation::Member();
    const auto memberType = member != nullptr ? party_member::PlayerTypeOf(member) : std::nullopt;
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    focus_scope::FocusScope scope(g_controller);
    if (!scope.Entered())
    {
        refusal = scope.Refusal();
        return SetupResult::Refused;
    }
    std::optional<leader_scope::LeaderScope>     leader;
    std::optional<leader_scope::LeaderTypeScope> type;
    if (memberType && memberWeak != 0)
    {
        leader.emplace(memberWeak);
        type.emplace(*memberType);
    }
    if (!call())
    {
        scope.Abandon();
        return SetupResult::Faulted;
    }
    return SetupResult::Ran;
}

SetupResult RunSetup(void* world, std::uint8_t& result, const wchar_t*& refusal) noexcept
{
    return InPlayerTwoScopes(refusal, [&] { return platform::CallGuarded(g_setup, result, g_controller, world); });
}

/// Logs the "setup done" bytes of both controllers and the deferred mark on change.
void WatchLifecycle() noexcept
{
    const auto one = Field<std::uintptr_t>(g_endUISystem + kGameController);
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    const std::array<std::uint8_t, 3> lifecycle{Field<std::uint8_t>(one + kSetupDone),
                                                Field<std::uint8_t>(g_endUISystem + kSetupDeferred),
                                                two != 0 ? Field<std::uint8_t>(two + kSetupDone) : std::uint8_t{0xFF}};
    if (lifecycle != g_loggedLifecycle)
    {
        g_loggedLifecycle = lifecycle;
        Line(std::format(L"setup done: player 1 {} (a setup deferred: {}), player 2 {} (follows the game's: {})",
                         lifecycle[0], lifecycle[1], lifecycle[2], YesNo(g_current)));
    }
}

void ReplayLine(const std::wstring& text) noexcept
{
    if (g_replayLines < kMaxReplayLines && platform::Enabled(platform::Level::Debug))
    {
        ++g_replayLines;
        platform::LogDebug(L"command menu two: {}", text);
    }
}

/// The game's call may be made again on player 2's controller: it is built, we are on the game thread, the
/// call was made on the game's controller (the UI system's slot names it), and, when the call needs player
/// 2's member (Setup stores the leader's type), player 2 plays.
bool CanFollow(void* gameController, bool needsPlayer) noexcept
{
    return g_installed && !g_failed && g_controller != nullptr && generation::OnGameThread() &&
           (!needsPlayer || generation::Active()) && gameController != nullptr && gameController != g_controller &&
           reinterpret_cast<std::uintptr_t>(gameController) == Field<std::uintptr_t>(g_endUISystem + kGameController);
}

/// The game's Members panel rebuild, for the mod's calls: the entry redirect's trampoline, or on 1.005 the function
/// itself (its two calls are redirected instead).
RefreshFn RefreshFunction() noexcept
{
    return g_refreshFn != nullptr ? g_refreshFn : reinterpret_cast<RefreshFn>(g_refresh.Original());
}

/// The Members panel of player 2's controller rebuilt from the party, in player 2's scopes.
void RefreshPlayerTwo(const wchar_t* why) noexcept
{
    const auto     refresh = RefreshFunction();
    const wchar_t* refusal = nullptr;
    switch (InPlayerTwoScopes(refusal, [&] { return platform::CallGuarded(refresh, g_controller); }))
    {
        case SetupResult::Ran:
            ReplayLine(std::format(L"player 2's Members panel rebuilt from the party ({})", why));
            break;
        case SetupResult::Refused:
            ReplayLine(std::format(L"player 2's Members panel NOT rebuilt ({}): {}", why, refusal));
            break;
        case SetupResult::Faulted:
            Line(std::format(L"FAILED: rebuilding player 2's Members panel faulted ({})", why));
            break;
    }
}

/// Replaces the Members panel's rebuild: the game's controller's, then player 2's (a rebuild of player 2's
/// own, from its Setup or from us, runs once).
void RefreshCall(void* controller) noexcept
{
    RefreshFunction()(controller);
    if (CanFollow(controller, false))
    {
        RefreshPlayerTwo(L"the game rebuilt its own");
    }
}

/// Replaces the squad switch's layout call: the game lays out its own panel, then player 2's is rebuilt
/// from the party the switch just changed. (The switch is a field-menu key; pad 2's field presses go to
/// player 1's menu, so the controller here is the game's.)
void SquadLayoutCall(void* controller, std::uint8_t all, std::uint32_t first, std::uint8_t force) noexcept
{
    g_layoutCells(controller, all, first, force);
    if (CanFollow(controller, false))
    {
        RefreshPlayerTwo(L"the game's in-field squad switch");
    }
    else if (controller != nullptr && controller == g_controller)
    {
        Line(L"WARNING: the in-field squad switch ran on player 2's controller; the game's panel is not rebuilt");
    }
}

/// Replaces the game's Setup call: the game's controller, then player 2's with the same world, in player 2's
/// focus context and with the leader answering player 2's member.
std::uint8_t SetupCall(void* controller, void* world) noexcept
{
    const std::uint8_t answer = reinterpret_cast<SetupFn>(g_setupCall.Original())(controller, world);
    g_current = false;
    g_tries   = 0;
    if (!CanFollow(controller, true))
    {
        ReplayLine(L"the game set up its controller; player 2's follows when player 2 plays (join Setup)");
        return answer;
    }
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    switch (RunSetup(world, set, refusal))
    {
        case SetupResult::Ran:
            g_current = true;
            ReplayLine(std::format(L"the game's Setup made on player 2's controller too (returned {}): 777={} 783={}", set,
                                   Field<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kSetupDone),
                                   Field<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kMenuLayer)));
            break;
        case SetupResult::Refused:
            ReplayLine(std::format(L"the game's Setup NOT made on player 2's controller: {}; the join Setup follows", refusal));
            break;
        case SetupResult::Faulted:
            g_failed = true;
            Line(L"FAILED: the game's Setup faulted on player 2's controller; player 2's menu, attack, guard and dodge "
                 L"stay off");
            break;
    }
    return answer;
}

/// Replaces the game's ResetAll call: the game's controller, then player 2's, in player 2's focus context
/// (ResetAll also resets the key guide the scope names, player 2's) and, when player 2 plays, with the
/// leader answering player 2's member. ResetAll needs no member, so player 2's follows it even while player
/// 2 is out (the field exit takes player 2 out first). Player 2's then waits for the game's next Setup, or
/// the join Setup, as the game's waits for its own.
void ResetAllCall(void* controller) noexcept
{
    reinterpret_cast<ResetAllFn>(g_resetAllCall.Original())(controller);
    g_current = false;
    g_tries   = 0;
    if (!CanFollow(controller, false))
    {
        ReplayLine(L"the game reset its controller; player 2's is not built yet");
        return;
    }
    const auto     resetAll = reinterpret_cast<ResetAllFn>(g_resetAllCall.Original());
    const wchar_t* refusal  = nullptr;
    switch (InPlayerTwoScopes(refusal, [&] { return platform::CallGuarded(resetAll, g_controller); }))
    {
        case SetupResult::Ran:
            ReplayLine(std::format(L"the game's ResetAll made on player 2's controller too: 777={}",
                                   Field<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kSetupDone)));
            break;
        case SetupResult::Refused:
            ReplayLine(std::format(L"the game's ResetAll NOT made on player 2's controller: {}", refusal));
            break;
        case SetupResult::Faulted:
            g_failed = true;
            Line(L"FAILED: the game's ResetAll faulted on player 2's controller; player 2's menu, attack, guard and dodge "
                 L"stay off");
            break;
    }
}

/// Allocates and constructs the controller, as EndUISystem__Ctor does for the game's.
bool Build() noexcept
{
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
    g_controller = memory;
    Line(std::format(L"built player 2's controller at {:#x}", reinterpret_cast<std::uintptr_t>(memory)));
    // Player 2's key guide, as EndUISystem__Ctor builds the game's. Optional: without it player 2's controller
    // leaves the shared one alone (player_two/command_menu_tick skips its key-guide writes).
    void* guide = nullptr;
    if (platform::CallGuarded(g_malloc, guide, kKeyGuideSize, std::uint32_t{0}) && guide != nullptr)
    {
        std::memset(guide, 0, kKeyGuideSize);
        void* built2 = nullptr;
        if (platform::CallGuarded(g_keyGuideCtor, built2, guide))
        {
            g_keyGuide = guide;
            focus_scope::SetKeyGuide(guide);
        }
    }
    Line(std::format(L"player 2's key guide {}", g_keyGuide != nullptr ? L"built" : L"NOT built (the shared one stays)"));
    return true;
}

Snapshot Read() noexcept
{
    Snapshot   now;
    const auto ctrl = reinterpret_cast<std::uintptr_t>(g_controller);
    for (std::size_t i = 0; i < now.members.size(); ++i)
    {
        now.members[i] = Field<std::int32_t>(ctrl + kMembers + i * 4);
    }
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = Field<std::uint8_t>(ctrl + kBytes[i]);
    }
    const auto weak = Field<std::uint64_t>(ctrl + kMenuWidget);
    if (weak != 0)
    {
        platform::CallGuarded(g_weakGet, now.widget, &weak);
    }
    now.widgetNamesUs = now.widget != nullptr &&
                        Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(now.widget) + kWidgetCtrl) == ctrl;
    now.context = focus_scope::PlayerTwoContext();
    return now;
}

void Watch() noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || g_lines >= kMaxLines)
    {
        return;
    }
    const Snapshot now = Read();
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_logged     = now;
    g_loggedOnce = true;
    const auto& b = now.bytes;
    ++g_lines;
    platform::LogDebug(L"command menu two: {}", std::format(L"members {} {} {} {} | menu {} (names this controller: {}) | 25C={} 258={} 777={} 783={} 5E1={} "
                     L"540={} | player 2's focus: path {}, layers {}",
                     now.members[0], now.members[1], now.members[2], now.members[3], NameOf(now.widget),
                     YesNo(now.widgetNamesUs), b[0], b[1], b[2], b[3], b[4], b[5], now.context.pathCount,
                     now.context.layerCount));
}

/// One call site redirected: at install on the research build, from the game thread on 1.005.
template <class Fn>
bool Place(platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, Fn* with) noexcept
{
    const auto replacement = reinterpret_cast<std::uintptr_t>(with);
    return g_fromGameThread ? call.InstallFromOwnThread(site, target, replacement) : call.Install(site, target, replacement);
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

/// The redirects, from what Install found (g_installed: its checks passed). At install on the research build; on
/// 1.005 from the game thread.
bool PlaceSites(const platform::GameImage& image) noexcept
{
    if (g_installed)
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
    }
    platform::Log(L"command menu two: {}", g_installed ? L"player 2's command controller is built once player 2 plays and "
                                                         L"follows the game's Setup, ResetAll and Members panel rebuild (logged)"
                                                       : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto malloc = platform::FindFunction(image, kMalloc);
    const auto ctor   = platform::FindFunction(image, kCtor);
    const auto setup  = platform::FindFunction(image, kSetup);
    const auto get    = platform::FindFunction(image, facts::kWeakObjectGet);
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
    const auto system = image.Rebase(kEndUISystem, kKeyGuideSlot + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    const auto site   = [&image](std::uintptr_t address) { return image.Rebase(address, 5); };
    const auto resetAll = image.Rebase(kResetAll);
    const auto refresh  = platform::FindFunction(image, kRefresh);
    const auto setupAt = site(kSetupCall), resetAt = site(kResetAllCall);
    g_installed = malloc && ctor && setup && get && g_worldClass != 0 && system && resetAll && refresh && setupAt &&
                  resetAt && focus_scope::Install(image);
    g_current = false;
    g_tries   = 0;
    if (g_installed)
    {
        g_malloc  = reinterpret_cast<MallocFn>(*malloc);
        g_ctor    = reinterpret_cast<CtorFn>(*ctor);
        g_setup   = reinterpret_cast<SetupFn>(*setup);
        g_weakGet = reinterpret_cast<WeakGetFn>(*get);
        g_sites   = {*resetAt, *resetAll, *setupAt, *refresh};
        if (platform::RunningBuild() == platform::BuildIndex::V1005)
        {
            // A squad layout call crosses a 16-byte block in 1.005, and the rebuild's entry too: every site is placed
            // from the game thread, between its ticks (player_two/game_thread_sites.h).
            g_fromGameThread = true;
            g_installed      = false;  // set again by the placement
            g_placeImage.emplace(image);
            const bool queued = game_thread_sites::Queue([]() noexcept {
                if (g_placeImage)
                {
                    const platform::GameImage image = *g_placeImage;  // a failed placement's Remove() empties g_placeImage
                    g_installed                     = true;
                    PlaceSites(image);
                }
            });
            platform::Log(L"command menu two: {}", queued ? L"its calls are placed from the game thread (1.005: two cross a 16-byte block)"
                                                          : L"UNAVAILABLE (no room to place its calls from the game thread)");
            return queued;
        }
    }
    return PlaceSites(image);
}

void Remove() noexcept
{
    g_installed = false;  // the controller stays: the game's GC list refers to it
    g_placeImage.reset();  // a placement still queued does nothing
    if (g_refresh.IsInstalled() && !g_refresh.Restore())
    {
        platform::Log(L"command menu two: the Members panel rebuild was changed after us; left in place (it passes through)");
    }
    for (auto* call : {&g_squadLayoutCalls[0], &g_squadLayoutCalls[1], &g_setupCall, &g_resetAllCall, &g_refreshTailCall,
                       &g_refreshSetupCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu two: a call was changed after us; left in place (it passes through)");
        }
    }
}

void* Controller() noexcept
{
    return g_controller;
}

void* KeyGuide() noexcept
{
    return g_keyGuide;
}

namespace {

/// The object a FWeakObjectPtr names, or null.
void* Resolve(std::uint64_t weak) noexcept
{
    void* object = nullptr;
    return weak != 0 && g_weakGet != nullptr && platform::CallGuarded(g_weakGet, object, &weak) ? object : nullptr;
}

/// A controller's widget (its FWeakObjectPtr at `offset`), or null.
void* WidgetOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    return controller != 0 ? Resolve(Field<std::uint64_t>(controller + offset)) : nullptr;
}

void SetOpacity(void* widget, float opacity) noexcept
{
    if (widget != nullptr && hud::widget_opacity::Get(widget) != opacity)
    {
        hud::widget_opacity::Set(widget, opacity);
    }
}

}  // namespace

void ShowOnly(int player) noexcept
{
    if (g_controller == nullptr || g_endUISystem == 0 || !hud::widget_opacity::Installed() || !generation::OnGameThread())
    {
        return;
    }
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    const auto one = Field<std::uintptr_t>(g_endUISystem + kGameController);
    if (player != 1 || Field<std::uint8_t>(two + kHudMode) == 0 || one == 0)
    {
        return Restore();
    }
    for (std::size_t i = 0; i <= kParts.size(); ++i)
    {
        // The last entry is the key guide: the game's (EndUISystem[53]) and player 2's, each widget at +4.
        // Player 2's shows at full opacity: its own tick (player_two/widget_owner) sets its visibility and
        // plays its show and hide animations.
        const bool guide     = i == kParts.size();
        const auto oneOwner  = guide ? Field<std::uintptr_t>(g_endUISystem + kKeyGuideSlot) : one;
        const auto twoOwner  = guide ? reinterpret_cast<std::uintptr_t>(g_keyGuide) : two;
        const auto offset    = guide ? kKeyGuideWidget : kParts[i].offset;
        const bool borrow    = !guide && kParts[i].borrowOpacity;
        void*      oneWidget = WidgetOf(oneOwner, offset);
        void*      twoWidget = WidgetOf(twoOwner, offset);
        if (oneWidget == nullptr || twoWidget == nullptr)
        {
            continue;
        }
        if (!g_swapped)
        {
            g_oneSaved[i] = {Field<std::uint64_t>(oneOwner + offset), hud::widget_opacity::Get(oneWidget)};
        }
        SetOpacity(twoWidget, borrow ? g_oneSaved[i].opacity : 1.0F);
        SetOpacity(oneWidget, 0.0F);
    }
    g_swapped = true;
}


void Restore() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (!hud::widget_opacity::Installed())
    {
        return;
    }
    if (g_swapped)
    {
        for (auto& saved : g_oneSaved)
        {
            SetOpacity(Resolve(saved.weak), saved.opacity);  // null (skipped) if the game rebuilt it
            saved = {};
        }
        g_swapped = false;
    }
    for (const Part& part : kParts)
    {
        SetOpacity(WidgetOf(reinterpret_cast<std::uintptr_t>(g_controller), part.offset), 0.0F);
    }
    SetOpacity(WidgetOf(reinterpret_cast<std::uintptr_t>(g_keyGuide), kKeyGuideWidget), 0.0F);
}

namespace {

/// After the join Setup: player 2's command menu gets player 2's UI focus again through the game's own
/// calls, in player 2's scopes, when the game's controller has battle input on. The game's
/// ResumeHudInput(0) as its field starts keeps battle input on, with its menu's focus layer up, through the
/// field and every battle. Player 2 missed it while out, and its controller outlives the leave with its
/// flag and layer byte (+0x783) still set, while the layer itself went with the focus of the player who
/// left. The battle-input mirror follows only a change, and the game's layer setter returns early while
/// the byte is set, so player 2's focus would stay off its menu: pad 2's Attack, Unique and Guard would
/// reach nothing (the user's retry tests, v0.28.0). The layer setter's off call, then ResumeHudInput(0)
/// (command_menu_tick::RefocusMenu). A written exception, as the lock sync: missed game calls made once at
/// the join.
void RefocusAtJoin(std::uintptr_t one) noexcept
{
    if (!command_menu_tick::BattleInputOn(reinterpret_cast<const void*>(one)))
    {
        return;
    }
    const auto     layer   = Field<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kMenuLayer);
    const wchar_t* refusal = nullptr;
    switch (InPlayerTwoScopes(refusal, [] { return command_menu_tick::RefocusMenu(g_controller); }))
    {
        case SetupResult::Ran:
            Line(std::format(L"join: player 2's menu focus layer {}put up again through the game's calls (layer byte "
                             L"was {}); battle input on",
                             layer != 0 ? L"taken down and " : L"", layer));
            break;
        case SetupResult::Refused:
            Line(std::format(L"join: player 2's menu focus NOT restored: {}", refusal));
            break;
        case SetupResult::Faulted:
            Line(L"join: restoring player 2's menu focus FAULTED");
            break;
    }
}

}  // namespace

void KeepHidden() noexcept
{
    if (g_controller == nullptr || g_weakGet == nullptr || !hud::widget_opacity::Installed() ||
        !generation::OnGameThread())
    {
        return;
    }
    for (const Part& part : std::array<Part, kParts.size() + 1>{kParts[0], kParts[1], kParts[2], Part{kKeyGuideWidget, false}})
    {
        const bool guide  = part.offset == kKeyGuideWidget;
        void*      widget = WidgetOf(reinterpret_cast<std::uintptr_t>(guide ? g_keyGuide : g_controller), part.offset);
        if (widget == nullptr || hud::widget_opacity::Get(widget) == 0.0F)
        {
            continue;
        }
        const bool hidden = hud::widget_opacity::Set(widget, 0.0F);
        if (++g_hides <= 5 || g_hides % 100 == 0)
        {
            Line(std::format(L"widget +{:#x} {} (render opacity 0; {} time(s))", part.offset,
                             hidden ? L"hidden" : L"could NOT be hidden", g_hides));
        }
    }
}

void Tick() noexcept
{
    if (!g_installed || g_failed || !generation::OnGameThread() || !generation::Active())
    {
        return;
    }
    WatchLifecycle();
    if (g_current)
    {
        Watch();
        return;
    }
    // The join Setup: player 2's controller is built or set up when player 2 plays and it does not follow
    // the game's current setup (player 2 joined, or was out or refused at the game's Setup). It follows the
    // game's controller: only once that one is set up and no Setup of its own is pending or deferred.
    const auto one = Field<std::uintptr_t>(g_endUISystem + kGameController);
    if (one == 0 || Field<std::uint8_t>(one + kSetupDone) == 0 || Field<std::uint8_t>(one + kSetupPending) != 0 ||
        Field<std::uint8_t>(g_endUISystem + kSetupDeferred) != 0)
    {
        return;
    }
    void* world = WorldOf(generation::PlayerOneController());
    if (world == nullptr)
    {
        return;
    }
    if (g_tries >= kSetupTries && ++g_waitTicks % kWaitGap != 0)
    {
        return;  // refused so far: paced tries, never given up
    }
    if (g_controller == nullptr && !Build())
    {
        g_failed = true;
        Line(L"FAILED to build player 2's controller; player 2's menu, attack, guard and dodge stay off");
        return;
    }
    ++g_tries;
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    const auto     setup   = RunSetup(world, set, refusal);
    if (setup == SetupResult::Refused)
    {
        if (g_tries <= kSetupTries || g_tries % 20 == 0)
        {
            Line(std::format(L"join setup waits: {} (try {})", refusal, g_tries));
        }
        return;
    }
    if (setup == SetupResult::Faulted)
    {
        g_failed = true;
        Line(L"FAILED: the join setup faulted; player 2's menu, attack, guard and dodge stay off");
        return;
    }
    g_current    = true;
    g_tries      = 0;
    g_loggedOnce = false;
    // After the game's Setup, field step 9 rebuilds the game's Members panel from the party; player 2's
    // missed it (player 2 was out), so it runs now, in the game's order.
    RefreshPlayerTwo(L"after the join Setup, as field step 9 follows the game's");
    command_locks::Sync(g_controller);
    RefocusAtJoin(one);
    // Only at a join (an object walk is slow): a second Setup must not leave extra menus behind.
    Line(std::format(L"join setup for world '{}' (returned {}); command menu widgets now: {}", NameOf(world), set,
                     CountMenus()));
    Watch();
}

}  // namespace lss::player_two::command_menu_two
