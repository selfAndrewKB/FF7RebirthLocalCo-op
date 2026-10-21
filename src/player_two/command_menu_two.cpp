#include "player_two/command_menu_two.h"

#include <atomic>
#include <intrin.h>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
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
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/menu_rules.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "player_two/weak_object.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

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
// The shortcut tables (RE-p2-shortcut-refresh.md): each controller keeps its own copy of the shortcut assignments
// (both sets, a row per battle ordinal, built from the save by MemberSelecter_RebuildShortcutTables on ctrl+0xF0);
// everything that shows or fires a shortcut reads the copy. The menus rebuild it right after each change through
// EndUI23_RebuildShortcutTables, which rebuilds EndUISystem[23]'s only (FieldCommandCtrl_RebuildShortcutTables:
// the rebuild, then +0x280 = -1, a repaint on the controller's next tick). Its entry, `mov eax, 28h` (5 bytes, inside
// one block on both builds), is redirected and the game's rebuild made again on the other controller. 1.004 also
// inlines it once (the status menu's member lambda, a call of the table builder); 1.005 calls the wrapper there.
// Item use rebuilds the executing controller's only (FieldCommandCtrl_ExecuteCommandRecord); its call is followed
// both ways (the inventory is shared). Each call inside one aligned 16-byte block.
constexpr std::uint8_t kRebuildShortcutsBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x86, 0xEC, 0x6D, 0xFE, 0x48, 0x2B, 0xE0,
                                                   0xE8, 0x8E, 0x66, 0xE3, 0xFC, 0x48, 0x8B, 0x88, 0xB8, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRebuildShortcuts{L"EndUI23_RebuildShortcutTables", 0x1439D02A0, kRebuildShortcutsBytes};
constexpr std::size_t            kRebuildShortcutsFirst = 5;
constexpr std::uint8_t kCtrlShortcutsBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x60, 0xB1, 0xB8, 0x00, 0x48,
                                                0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1, 0xF0, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kCtrlShortcuts{L"FieldCommandCtrl_RebuildShortcutTables", 0x141523DC4, kCtrlShortcutsBytes};
constexpr std::uint8_t kSelecterShortcutsBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                    0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSelecterShortcuts{L"MemberSelecter_RebuildShortcutTables", 0x140D202B4,
                                                    kSelecterShortcutsBytes};
constexpr std::uintptr_t kStatusShortcutsCall = 0x1422FA6D9;  // 1.004 only: call of the table builder (inlined wrapper)
constexpr std::uintptr_t kItemShortcutsCall   = 0x141524C69;  // in ExecuteCommandRecord: call of the controller's rebuild
constexpr std::uint32_t  kSelecter            = 0xF0;         // the member selecter, embedded
constexpr std::uint32_t  kShortcutSets        = 0x208;        // selecter +0x118: two TArray<TArray<88-byte record>>
constexpr std::uint32_t  kShortcutTarget      = 0x3C;         // the ordinal whose shortcuts show
constexpr std::uint32_t  kShortcutShown       = 0x280;        // the last target shown (-1: repaint)
constexpr std::size_t    kRecordSize          = 88;
constexpr std::uint32_t  kRecordCommand       = 56;
constexpr int            kMaxShortcutLines    = 60;

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
using ShortcutsFn    = void (*)(void* controller);  // FieldCommandCtrl_RebuildShortcutTables
using SelecterShortcutsFn = void (*)(void* selecter);  // MemberSelecter_RebuildShortcutTables

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
platform::EntryRedirect g_rebuildShortcuts;
platform::CallRedirect  g_statusShortcutsCall, g_itemShortcutsCall;
ShortcutsFn             g_ctrlShortcuts     = nullptr;  // the game's rebuild of one controller's tables
SelecterShortcutsFn     g_selecterShortcuts = nullptr;  // 1.004: the status-menu call's target
std::uintptr_t          g_lastGame          = 0;        // the game's controller, as last read outside player 2's scope
int                     g_shortcutLines     = 0;
std::optional<platform::GameImage> g_image;             // for the logged return addresses
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t resetAt = 0, resetAll = 0, setupAt = 0, refresh = 0;
    // The shortcut follows (optional: without them only the shortcuts stay as they were).
    std::uintptr_t rebuildShortcuts = 0, statusAt = 0, itemAt = 0;
};
Sites                              g_sites;
platform::DeferredSites g_placement;  // 1.005: the calls placed from the game thread (platform/deferred_sites.h)
constexpr const wchar_t* kUnavailable = L"UNAVAILABLE (a check failed)";

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

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"command menu two: {}", text);
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
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kGameController);
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    const std::array<std::uint8_t, 3> lifecycle{ReadAt<std::uint8_t>(one + kSetupDone),
                                                ReadAt<std::uint8_t>(g_endUISystem + kSetupDeferred),
                                                two != 0 ? ReadAt<std::uint8_t>(two + kSetupDone) : std::uint8_t{0xFF}};
    if (lifecycle != g_loggedLifecycle)
    {
        g_loggedLifecycle = lifecycle;
        Line(std::format(L"setup done: player 1 {} (a setup deferred: {}), player 2 {} (follows the game's: {})",
                         lifecycle[0], lifecycle[1], lifecycle[2], YesNo(g_current)));
    }
}

void ReplayLine(const std::wstring& text) noexcept
{
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"command menu two: {}", text);
    }
}

/// The game's call may be made again on player 2's controller: it is built, we are on the game thread, the
/// call was made on the game's controller (the UI system's slot names it), and, when the call needs player
/// 2's member (Setup stores the leader's type), player 2 plays.
bool CanFollow(void* gameController, bool needsPlayer) noexcept
{
    return menu_rules::CanFollow(g_installed, g_failed, g_controller != nullptr, generation::OnGameThread(), needsPlayer,
                                 generation::Active(), gameController != nullptr, gameController == g_controller,
                                 reinterpret_cast<std::uintptr_t>(gameController) ==
                                     ReadAt<std::uintptr_t>(g_endUISystem + kGameController));
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

/// The game's controller: the UI system's slot, or inside player 2's scope (the slot names player 2's) the one last
/// read outside it.
std::uintptr_t GameController() noexcept
{
    const auto slot = ReadAt<std::uintptr_t>(g_endUISystem + kGameController);
    if (slot != 0 && slot != reinterpret_cast<std::uintptr_t>(g_controller))
    {
        g_lastGame = slot;
    }
    return g_lastGame;
}

/// The command ids (record +56) of the six assignable shortcuts in `row` of a controller's set, as text.
std::wstring ShortcutIds(std::uintptr_t controller, int set, std::int32_t row) noexcept
{
    const auto table = controller + kShortcutSets + 16 * set;
    const auto rows  = ReadAt<std::uintptr_t>(table);
    if (rows == 0 || row < 0 || row >= ReadAt<std::int32_t>(table + 8))
    {
        return L"[none]";
    }
    const auto records = ReadAt<std::uintptr_t>(rows + 16 * row);
    const auto count   = ReadAt<std::int32_t>(rows + 16 * row + 8);
    std::wstring text  = L"[";
    for (std::int32_t r = 0; r < 6 && r < count && records != 0; ++r)
    {
        text += std::format(L"{}{}", r != 0 ? L" " : L"",
                            ReadAt<std::uint32_t>(records + kRecordSize * r + kRecordCommand));
    }
    return text + L"]";
}

/// The game's rebuild of `target`'s shortcut tables, after the game rebuilt `source`'s (`why`, called from
/// `caller`). The rebuild reads the save and no leader or UI slot, so it needs no scope. Logged: both controllers'
/// ids in the shown row, which must match afterwards.
void FollowShortcuts(std::uintptr_t source, std::uintptr_t target, const wchar_t* why, std::uintptr_t caller) noexcept
{
    if (g_ctrlShortcuts == nullptr || target == 0 || target == source)
    {
        return;
    }
    const bool log   = g_shortcutLines < kMaxShortcutLines && platform::Enabled(platform::Level::Debug);
    const auto row   = ReadAt<std::int32_t>(target + kShortcutTarget);
    const auto ids   = [row](std::uintptr_t c, int set) { return ShortcutIds(c, set, row); };
    const auto before = log ? ids(target, 0) + L" " + ids(target, 1) : std::wstring{};
    if (!platform::CallGuarded(g_ctrlShortcuts, reinterpret_cast<void*>(target)))
    {
        Line(std::format(L"FAILED: rebuilding the shortcut tables of {}'s controller faulted ({})",
                         target == reinterpret_cast<std::uintptr_t>(g_controller) ? L"player 2" : L"player 1", why));
        return;
    }
    if (log)
    {
        ++g_shortcutLines;
        const auto source0 = ids(source, 0), source1 = ids(source, 1), after0 = ids(target, 0), after1 = ids(target, 1);
        platform::LogDebug(L"command menu two: {}'s shortcut tables rebuilt too ({}, from 0x{:X}): row {}: the rebuilt "
                           L"one {} {} (was {}), the other {} {} (match: {}); +0x280={}",
                           target == reinterpret_cast<std::uintptr_t>(g_controller) ? L"player 2" : L"player 1", why,
                           g_image ? g_image->ResearchAddress(caller) : caller, row, after0, after1, before, source0,
                           source1, YesNo(after0 == source0 && after1 == source1),
                           ReadAt<std::int32_t>(target + kShortcutShown));
    }
}

/// Replaces EndUI23_RebuildShortcutTables: the game rebuilds its slot's controller, then the other controller is
/// rebuilt too. Outside player 2's scope that is player 2's; inside it (an event in player 2's pass) the game's.
void RebuildShortcutsCall() noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto ran    = ReadAt<std::uintptr_t>(g_endUISystem + kGameController);
    reinterpret_cast<void (*)()>(g_rebuildShortcuts.Original())();
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    if (CanFollow(reinterpret_cast<void*>(ran), false))
    {
        FollowShortcuts(ran, two, L"the game rebuilt its shortcut tables", caller);
    }
    else if (ran != 0 && ran == two && g_installed && !g_failed && generation::OnGameThread())
    {
        FollowShortcuts(two, GameController(), L"the game's shortcut rebuild ran in player 2's scope", caller);
    }
}

/// 1.004 only: replaces the status menu's call of the table builder (an inlined EndUI23_RebuildShortcutTables on the
/// game's controller), then rebuilds player 2's.
void StatusShortcutsCall(void* selecter) noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_selecterShortcuts(selecter);
    const auto controller = reinterpret_cast<std::uintptr_t>(selecter) - kSelecter;
    if (CanFollow(reinterpret_cast<void*>(controller), false))
    {
        FollowShortcuts(controller, reinterpret_cast<std::uintptr_t>(g_controller),
                        L"the status menu rebuilt the game's shortcut tables", caller);
    }
}

/// Replaces the item use's rebuild of the executing controller's tables, then rebuilds the other's: the inventory is
/// shared, so either player's item changes both players' shortcuts.
void ItemShortcutsCall(void* controller) noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_ctrlShortcuts(controller);
    const auto ran = reinterpret_cast<std::uintptr_t>(controller), two = reinterpret_cast<std::uintptr_t>(g_controller);
    if (CanFollow(controller, false))
    {
        FollowShortcuts(ran, two, L"an item use rebuilt the game's", caller);
    }
    else if (ran != 0 && ran == two && g_installed && !g_failed && generation::OnGameThread())
    {
        FollowShortcuts(two, GameController(), L"player 2's item use rebuilt player 2's", caller);
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
                                   ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kSetupDone),
                                   ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kMenuLayer)));
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
                                   ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kSetupDone)));
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
        now.members[i] = ReadAt<std::int32_t>(ctrl + kMembers + i * 4);
    }
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = ReadAt<std::uint8_t>(ctrl + kBytes[i]);
    }
    now.widget = weak_object::ObjectOf(ReadAt<std::uint64_t>(ctrl + kMenuWidget));
    now.widgetNamesUs = now.widget != nullptr &&
                        ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(now.widget) + kWidgetCtrl) == ctrl;
    now.context = focus_scope::PlayerTwoContext();
    return now;
}

void Watch() noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
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

/// The shortcut follows, from what Install found; each optional (a failed one leaves only that source unfollowed).
void PlaceShortcuts(const platform::GameImage& image) noexcept
{
    const Sites& s = g_sites;
    if (g_ctrlShortcuts == nullptr || s.rebuildShortcuts == 0)
    {
        platform::Log(L"command menu two: player 2's shortcut tables do NOT follow the game's rebuilds (a check failed); "
                      L"they change at the next Members panel rebuild");
        return;
    }
    const bool menus = g_rebuildShortcuts.Install(
        s.rebuildShortcuts,
        image.EntryInstruction(kRebuildShortcuts.address, std::span<const std::uint8_t>(kRebuildShortcutsBytes, kRebuildShortcutsFirst)),
        reinterpret_cast<std::uintptr_t>(&RebuildShortcutsCall));
    const bool status = s.statusAt != 0 && g_selecterShortcuts != nullptr &&
                        Place(g_statusShortcutsCall, s.statusAt, reinterpret_cast<std::uintptr_t>(g_selecterShortcuts),
                              &StatusShortcutsCall);
    const bool items = s.itemAt != 0 &&
                       Place(g_itemShortcutsCall, s.itemAt, reinterpret_cast<std::uintptr_t>(g_ctrlShortcuts), &ItemShortcutsCall);
    platform::Log(L"command menu two: the shortcut tables follow the game's rebuilds: menus and character events {}, the "
                  L"status menu's inlined one {}, item use {} (logged at Debug)",
                  menus ? L"yes" : L"NO", platform::RunningBuild() == platform::BuildIndex::V1005 ? L"(1.005 calls the wrapper)"
                                                                                                 : (status ? L"yes" : L"NO"),
                  items ? L"yes" : L"NO");
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
        PlaceShortcuts(image);
    }
    platform::Log(L"command menu two: {}", g_installed ? L"player 2's command controller is built once player 2 plays and "
                                                         L"follows the game's Setup, ResetAll and Members panel rebuild (logged)"
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
    const auto system = image.Rebase(kEndUISystem, kKeyGuideSlot + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    const auto site   = [&image](std::uintptr_t address) { return image.Rebase(address, 5); };
    const auto resetAll = image.Rebase(kResetAll);
    const auto refresh  = platform::FindFunction(image, kRefresh);
    const auto setupAt = site(kSetupCall), resetAt = site(kResetAllCall);
    g_installed = malloc && ctor && setup && weak_object::Known() && g_worldClass != 0 && system && resetAll &&
                  refresh && setupAt && resetAt && focus_scope::Install(image);
    g_current = false;
    g_tries   = 0;
    if (g_installed)
    {
        g_malloc  = reinterpret_cast<MallocFn>(*malloc);
        g_ctor    = reinterpret_cast<CtorFn>(*ctor);
        g_setup   = reinterpret_cast<SetupFn>(*setup);
        g_sites   = {*resetAt, *resetAll, *setupAt, *refresh};
        g_image.emplace(image);
        // The shortcut follows: the wrapper and the controller's rebuild on both builds; the status menu's inlined
        // rebuild on the research build only.
        const auto wrapper = platform::FindFunction(image, kRebuildShortcuts);
        const auto rebuild = platform::FindFunction(image, kCtrlShortcuts);
        g_ctrlShortcuts    = rebuild ? reinterpret_cast<ShortcutsFn>(*rebuild) : nullptr;
        g_sites.rebuildShortcuts = wrapper ? *wrapper : 0;
        g_sites.itemAt           = site(kItemShortcutsCall).value_or(0);
        if (platform::RunningBuild() != platform::BuildIndex::V1005)
        {
            const auto builder  = platform::FindFunction(image, kSelecterShortcuts);
            g_selecterShortcuts = builder ? reinterpret_cast<SelecterShortcutsFn>(*builder) : nullptr;
            g_sites.statusAt    = site(kStatusShortcutsCall).value_or(0);
        }
    }
    if (!g_installed)
    {
        platform::Log(L"command menu two: {}", kUnavailable);
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
        platform::Log(L"command menu two: {}",
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
        platform::Log(L"command menu two: the Members panel rebuild was changed after us; left in place (it passes through)");
    }
    if (g_rebuildShortcuts.IsInstalled() && !g_rebuildShortcuts.Restore())
    {
        platform::Log(L"command menu two: the shortcut rebuild was changed after us; left in place (it passes through)");
    }
    for (auto* call : {&g_squadLayoutCalls[0], &g_squadLayoutCalls[1], &g_setupCall, &g_resetAllCall, &g_refreshTailCall,
                       &g_refreshSetupCall, &g_statusShortcutsCall, &g_itemShortcutsCall})
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

/// A controller's widget (its FWeakObjectPtr at `offset`), or null.
void* WidgetOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    return controller != 0 ? weak_object::ObjectOf(ReadAt<std::uint64_t>(controller + offset)) : nullptr;
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
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kGameController);
    if (player != 1 || ReadAt<std::uint8_t>(two + kHudMode) == 0 || one == 0)
    {
        return Restore();
    }
    for (std::size_t i = 0; i <= kParts.size(); ++i)
    {
        // The last entry is the key guide: the game's (EndUISystem[53]) and player 2's, each widget at +4.
        // Player 2's shows at full opacity: its own tick (player_two/widget_owner) sets its visibility and
        // plays its show and hide animations.
        const bool guide     = i == kParts.size();
        const auto oneOwner  = guide ? ReadAt<std::uintptr_t>(g_endUISystem + kKeyGuideSlot) : one;
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
            g_oneSaved[i] = {ReadAt<std::uint64_t>(oneOwner + offset), hud::widget_opacity::Get(oneWidget)};
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
            SetOpacity(weak_object::ObjectOf(saved.weak), saved.opacity);  // null (skipped) if the game rebuilt it
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
    const auto     layer   = ReadAt<std::uint8_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kMenuLayer);
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
    if (g_controller == nullptr || !weak_object::Known() || !hud::widget_opacity::Installed() ||
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
    GameController();  // kept for a shortcut rebuild that runs in player 2's scope
    if (g_current)
    {
        Watch();
        return;
    }
    // The join Setup: player 2's controller is built or set up when player 2 plays and it does not follow
    // the game's current setup (player 2 joined, or was out or refused at the game's Setup). It follows the
    // game's controller: only once that one is set up and no Setup of its own is pending or deferred.
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kGameController);
    if (one == 0 || !menu_rules::GameSetUp(one, ReadAt<std::uint8_t>(one + kSetupDone),
                                           ReadAt<std::uint8_t>(one + kSetupPending),
                                           ReadAt<std::uint8_t>(g_endUISystem + kSetupDeferred)))
    {
        return;
    }
    void* world = WorldOf(generation::PlayerOneController());
    if (world == nullptr)
    {
        return;
    }
    if (!menu_rules::JoinTryDue(g_tries, g_waitTicks))
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
