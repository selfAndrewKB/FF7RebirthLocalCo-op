#include "player_two/command_menu_tick.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/command_menu_two.h"
#include "player_two/command_rules.h"
#include "player_two/engine_facts.h"
#include "player_two/focus_scope.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_on.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::command_menu_tick {

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
constexpr std::uint32_t kSetupPending   = 0x775;  // u8: the tick re-runs Setup
// FieldCommandCtrl_SetMenuFocusLayer783(ctrl, on): the menu's bottom focus layer (priority -1000),
// pushed and focused, or popped (RE-p2-command-menu-input.md §2).
constexpr std::uint8_t kSetMenuLayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18,
                                               0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x73, 0x80, 0x42,
                                               0x01, 0x48, 0x2B, 0xE0, 0x8A, 0x81, 0x83, 0x07, 0x00, 0x00};
constexpr platform::FunctionFact kSetMenuLayer{L"FieldCommandCtrl_SetMenuFocusLayer783", 0x140C86EA8, kSetMenuLayerBytes};
// Action keys the route keeps away from player 2's menu: target switching (the menu's handler would
// switch the battle system's target; player_two/lock_on already switches player 2's). An action
// event's key name is at +0x18 (the handler compares it with the FKeys' first eight bytes).
constexpr std::uintptr_t kPrevTargetKey  = 0x1491E46B8;
constexpr std::uintptr_t kNextTargetKey  = 0x1491E46D0;
constexpr std::uintptr_t kCommandMenuKey = 0x1491E44F0;
constexpr std::uint32_t  kEventKey       = 0x18;
// UEndUserWidget's SetVisibility (vt+0x280: the setter, then its listeners) and UWidget::Visibility
// (ESlateVisibility, u8 at +0xC3). Setup makes a menu Visible only when the controller's battle input
// is live (+0x25C), which player 2's is not when it is built out of battle; a Collapsed menu is cut
// from every focus path.
constexpr std::uint8_t kSetVisibilityBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                                0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetVisibility{L"UEndUserWidget_Vt280_SetVisibility_Notify", 0x140C27914,
                                                kSetVisibilityBytes};
// The input processor's key-up handler (IInputProcessor slot 3; FKeyEvent in r8, its UserIndex, the
// pad's controller id, at +0x0C) and the up route's call in ActionUp_UpdateTables (bool(owner, event)):
// RE-p2-command-menu-input.md §3.3, §10 Stage 3d.
// The handler's first instruction (mov [rsp+8], rbx) is often patched by another hook (UE4SS's input
// handling puts a jump there, live), so the slot is checked to still name the handler and the bytes
// are checked after that instruction.
constexpr std::uintptr_t kKeyUpEntry     = 0x145D40598;  // the vtable entry
constexpr std::uintptr_t kKeyUp          = 0x140E65D18;  // SquareInput__HandleKeyUpEvent
constexpr std::uintptr_t kKeyUpAfterFirst = 0x140E65D1D;
constexpr std::uint8_t   kKeyUpRestBytes[] = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x41,
                                              0x56, 0xB8, 0x60, 0x00, 0x00, 0x00, 0xE8};
constexpr std::uint32_t kKeyEventUser = 0x0C;
constexpr std::uint8_t  kUpRouteBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                           0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x60, 0xFE, 0xFF, 0xFF};
constexpr platform::FunctionFact kUpRoute{L"SquareInput__RouteActionUpToUIHandlerStack", 0x140E66A14, kUpRouteBytes};
constexpr std::uintptr_t kUpRouteCall = 0x140E66914;
constexpr std::uint32_t kVisibility = 0xC3;
constexpr std::uint32_t kMenuWidget = 0x4F0;  // the controller's menu (FWeakObjectPtr)
constexpr std::uint32_t kFieldMenu  = 0x4E8;  // its field menu, which holds the party list (FWeakObjectPtr)
// The battle system's command locks, written into the game's controller only (EndUISystem[23]):
// commands enabled (+0x5E2: every row is greyed while 0), commands locked (+0x5E3), and the event and
// tutorial locks per command category (u8 array at +0x2D8, count at +0x2E0) (RE-p2-command-menu-views.md §10.2).
constexpr std::uint32_t kCommandsEnabled = 0x5E2;
constexpr std::uint32_t kCommandsLocked  = 0x5E3;
constexpr std::uint32_t kCategoryLocks   = 0x2D8;
constexpr std::uint32_t kCategoryCount   = 0x2E0;
constexpr std::int32_t  kMaxCategories   = 32;
// The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by both controllers' menus
// (RE-p2-command-menu-tint.md). EnableType/DisableType are a latch over a real reference count, so two
// owners cancel each other; the game's other menus share a filter through the count itself
// (EndUIFilter_RefInc/RefDec on EndUISystem[41], keeping their own "holds it" byte). Player 2's menu does
// the same; player 1's keeps the latch. The wait sound's off is one mode slot: it is skipped while the
// other menu is still in wait mode.
constexpr std::uint8_t kFilterOnBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE8, 0xD3, 0xEB, 0x00};
constexpr std::uint8_t kFilterOffBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xF0, 0xDA, 0xEB, 0x00};
constexpr std::uint8_t kMenuSoundBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xE8, 0x50, 0x51, 0x01};
constexpr std::uint8_t kRefIncBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                            0x00, 0x00, 0x00, 0xE8, 0xAC, 0xD3, 0xEB, 0x00};
constexpr std::uint8_t kRefDecBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                            0x00, 0x00, 0x00, 0xE8, 0x74, 0xDA, 0xEB, 0x00};
constexpr platform::FunctionFact kFilterOn{L"EndUIFilter_EnableType", 0x1411F1B3C, kFilterOnBytes};
constexpr platform::FunctionFact kFilterOff{L"EndUIFilter_DisableType", 0x1411F1434, kFilterOffBytes};
constexpr platform::FunctionFact kMenuSound{L"EndSound_PostMenuEventIdx", 0x140B99E3C, kMenuSoundBytes};
constexpr platform::FunctionFact kRefInc{L"EndUIFilter_RefInc", 0x1411F1B74, kRefIncBytes};
constexpr platform::FunctionFact kRefDec{L"EndUIFilter_RefDec", 0x1411F14AC, kRefDecBytes};
constexpr std::uintptr_t kFilterOnCall  = 0x140963943;  // wait mode 0 -> 1
constexpr std::uintptr_t kFilterOffCall = 0x140963839;  // 5 -> 4
constexpr std::uintptr_t kSoundOffCall  = 0x14096384B;  // 4 -> 0 (event 13, the audio mode cleared)
constexpr std::uint32_t  kFilterManager = 0x148;        // EndUISystem[41]
constexpr std::uint32_t  kFilterLatch   = 0x1C;         // u8 per type
constexpr std::uint32_t  kFilterCount   = 0x2C;         // i32 per type
constexpr std::uint8_t   kTintType      = 0;

// The action keys (FKeys) the route and its log compare an event's key name with.
enum Action : std::size_t
{
    kAttack,
    kUnique,
    kGuard,
    kDodge,
    kActions
};
constexpr std::array<std::uintptr_t, kActions> kActionKeys{0x1491E44D8, 0x1491E4520, 0x1491E4598, 0x1491E45E0};

// The UI system's slots: the input object [2], the command controller [23], [50] (its +0x20 and +0x2C
// are written by the member selecter's tick). The input object's +0x64 is written by the same tick.
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kInput       = 0x10;
constexpr std::uint32_t  kController  = 0xB8;
constexpr std::uint32_t  kSlot50      = 0x190;
constexpr std::uint32_t  kSlot50Mode  = 0x20;
constexpr std::uint32_t  kSlot50Other = 0x2C;
constexpr std::uint32_t  kInputByte64 = 0x64;
// The party manager's leader type (u8; PM+0x148 is the leader's weak pointer, player_two/leader_scope).
constexpr std::uint32_t kLeaderType = 0x150;
constexpr std::uint32_t kPrimary    = 0x140;
constexpr std::uint32_t kLeaderWeak = 0x148;

// The controller (RE-p2-command-menu-tick.md §1, §2, §7).
constexpr std::uint32_t kStoredLeaderType = 0x30;   // u8
constexpr std::uint32_t kMembers          = 0x34;   // i32 x2: leader ordinal, ally
constexpr std::uint32_t kBattleInput      = 0x25C;  // u8
constexpr std::uint32_t kSwitchingLayer   = 0x258;  // u8
constexpr std::uint32_t kRequests         = 0x570;  // TArray<int32> {data, num}
constexpr std::uint32_t kHudMode          = 0x540;  // u8
constexpr std::uint32_t kBlocked          = 0x541;  // u8
constexpr std::uint32_t kReady            = 0x5E1;  // u8
constexpr std::uint32_t kWaitFilter       = 0x644;  // i32
constexpr std::uint32_t kPinch            = 0x650;  // u8
constexpr std::uint32_t kGuardLatch       = 0x760;  // i32
constexpr std::uint32_t kHudSoundMode     = 0x774;  // u8
constexpr std::uint32_t kReadyB           = 0x782;  // u8
constexpr std::uint32_t kMenuLayer        = 0x783;  // u8
constexpr std::array<std::uint32_t, 4> kLoggedGates{0x776, 0x77C, 0x77F, 0x787};  // u8, logged only
enum Request : std::int32_t
{
    kPrevMember = 0,
    kNextMember = 1,
    kReqAttack  = 12,
    kReqUnique  = 13,
    kReqGuardDodge = 32,
    kReqGuard   = 37,
    kReqSynergyPane = 27,  // set on Guard's press (the synergy pane at top level); the up route clears it
};
constexpr std::array<std::int32_t, 11> kLoggedRequests{0, 1, 5, 6, 12, 13, 25, 32, 37, 38, 39};
constexpr std::int32_t kDodgePress    = 3;  // req[32]: a dodge press
constexpr std::int32_t kDodgeGuarding = 4;  // req[32]: a dodge press while guarding
constexpr std::int32_t kGuardHeld     = 1;  // req[32]: guard held

constexpr std::size_t kSnapshotTypes = 15;
constexpr int         kMaxLines      = 400;  // events
constexpr int         kMaxWatchLines = 800;  // player 2's controller's state, on change
constexpr std::uint32_t kOpenPending = 0x5D0;  // i32: a menu open requested (>= 0), pushed 0.125 s later
constexpr std::uint32_t kGate777     = 0x777;  // u8: [UNKNOWN] 1 on the game's controller; 0 on player 2's after a reset

using TickFn      = void (*)(void* controller, float deltaTime);
using SlotFn      = void (*)(std::uint8_t slot, float value);
using SnapshotFn  = void (*)(void* queue, const std::uint64_t* commands, const std::uint8_t* categories);
using GuideShowFn = std::uint8_t (*)(void* guide, std::uint8_t mode);
using GuideMode8Fn = void (*)();
using HudModeFn   = void (*)(void* controller, std::uint8_t mode);
using ResumeHudInputFn = void (*)(void* controller, std::uint8_t kind);
using FilterOnFn       = void (*)(void* world, std::uint8_t type);
using FilterOffFn      = void (*)(std::uint8_t type);
using MenuSoundFn      = void (*)(std::uint8_t index);
using FilterCountFn    = void (*)(void* manager, std::uint8_t type);
using SuspendHudInputFn = void (*)(std::uint8_t kind, std::uint8_t reset);
using PartyListInputFn = void (*)(void* partyList, std::uint8_t on, std::uint8_t applyVisibility);
using LeaderFn         = void* (*)();
using SetMenuLayerFn = void (*)(void* controller, std::uint8_t on);
using SetVisibilityFn = void (*)(void* widget, std::uint8_t visibility);
using WeakGetFn       = void* (*)(const std::uint64_t* weak);
using KeyUpFn         = std::uint8_t (*)(void* self, void* application, const void* keyEvent);
using UpRouteFn       = std::uint8_t (*)(void* owner, void* event);

// Set once at install.
platform::CallRedirect g_tickCall, g_slotCall, g_snapshotCall, g_guideShowCall, g_guideMode8Call, g_upRouteCall;
platform::CallRedirect g_filterOnCall, g_filterOffCall, g_soundOffCall;
FilterCountFn          g_refInc = nullptr, g_refDec = nullptr;
platform::VTableEntry  g_keyUpSlot;
HudModeFn      g_hudMode   = nullptr;
ResumeHudInputFn  g_resumeHudInput  = nullptr;
SuspendHudInputFn g_suspendHudInput = nullptr;
PartyListInputFn g_partyListInput = nullptr;
LeaderFn         g_resolveLeader  = nullptr;
std::array<std::uintptr_t, kActions> g_keys{};
std::uintptr_t g_endUISystem  = 0;
std::uintptr_t g_partyManager = 0;  // the address holding the party manager
SetMenuLayerFn g_setMenuLayer = nullptr;
SetVisibilityFn g_setVisibility = nullptr;
WeakGetFn      g_weakGet = nullptr;
std::uintptr_t g_prevTargetKey = 0, g_nextTargetKey = 0, g_commandMenuKey = 0;

// Game thread.
bool         g_installed = false;
int          g_pass      = 0;  // 1: the game's controller ticks; 2: player 2's
std::uint64_t g_frame     = 0;  // ticks of the game's controller
std::uint64_t g_oneFrame  = 0;  // the frame player 1's slot value is from
float        g_oneSlot   = 1.0F;
float        g_twoSlot   = 1.0F;
float        g_written   = 1.0F;
bool         g_passed    = false;  // player 2's controller ticked this frame
int          g_lines     = 0;
int          g_watchLines = 0;
unsigned     g_visibilitySets = 0;  // visibility copies made (the tick can undo them every frame)
unsigned     g_refusals  = 0, g_shieldHits = 0, g_guideSkips = 0, g_snapshotEntries = 0;
std::uint64_t g_passes     = 0;  // player 2's passes run
unsigned     g_routed      = 0;  // pad 2's presses dispatched in player 2's focus context
std::array<unsigned, 5> g_routedKnown{};  // per action the log counts on its own (KnownAction)
bool         g_routeFailed = false;
bool         g_twoHoldsTint = false;  // player 2's menu holds one count of the tint (the game's own owner-byte pattern)
int          g_tintLines    = 0;
unsigned     g_upRouted    = 0;   // pad 2's releases routed to player 2's menu
std::int32_t g_upPad       = -1;  // the pad of the release being handled (key-up handler), or -1

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

template <class T>
bool Put(std::uintptr_t address, T value) noexcept
{
    return address != 0 && platform::WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(T));
}

void WatchLine(const std::wstring& text) noexcept
{
    if (g_watchLines < kMaxWatchLines)
    {
        ++g_watchLines;
        platform::Log(L"command menu tick: {}", text);
    }
}

void Line(const std::wstring& text) noexcept
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"command menu tick: {}", text);
    }
}

std::int32_t RequestOf(std::uintptr_t controller, std::int32_t index) noexcept
{
    const auto data = Field<std::uintptr_t>(controller + kRequests);
    return data != 0 && index < Field<std::int32_t>(controller + kRequests + 8)
               ? Field<std::int32_t>(data + 4 * static_cast<std::uint32_t>(index))
               : -1;
}

void SetRequest(std::uintptr_t controller, std::int32_t index, std::int32_t value) noexcept
{
    const auto data = Field<std::uintptr_t>(controller + kRequests);
    if (data != 0 && index < Field<std::int32_t>(controller + kRequests + 8))
    {
        Put(data + 4 * static_cast<std::uint32_t>(index), value);
    }
}

/// The held action requests (Attack, Unique, Guard with its dodge slot and the synergy pane) of player 2's
/// controller cleared, as the game's up handler clears them on the matching release.
void ClearHeld(std::uintptr_t two) noexcept
{
    for (const std::int32_t request : {kReqAttack, kReqUnique, kReqGuard, kReqGuardDodge, kReqSynergyPane})
    {
        SetRequest(two, request, 0);
    }
}

/// Before player 2's tick. Pad 2's actions reach player 2's controller through the game's own input
/// handler (pad 2's presses and releases dispatched in player 2's UI): the press sets the request and
/// the release clears it, as for player 1. Here only: the leader-switch requests cleared (player 2's d-pad
/// never switches the party leader), and the held requests cleared while player 1's battle input is
/// suspended (a battle event or help menu stops player 1's the same way, by taking the menu's events).
void WriteRequests(std::uintptr_t two, std::uintptr_t one) noexcept
{
    SetRequest(two, kPrevMember, 0);
    SetRequest(two, kNextMember, 0);
    const bool live = Field<std::uint8_t>(one + kBattleInput) != 0 && Field<std::uint8_t>(one + kMenuLayer) != 0 &&
                      Field<std::uint8_t>(two + kHudMode) != 0;
    if (!live)
    {
        ClearHeld(two);
    }
}

/// Battle input live (+0x25C = the member selecter's input flag, ctrl+0xF0+0x16C): the game turns it on and
/// off with EndUI23_ResumeHudInput(0) / SuspendHudInput(0, 0), on its own controller only; the same calls are
/// made for player 2's (the suspend inside player 2's UI, where the UI system's controller slot names player
/// 2's). Without it the input handler drops Attack, Unique and Guard, and the tick clears every request
/// (RE-p2-command-menu-actions.md; RE-p2-command-menu-lifecycle.md §3.3).
void MirrorBattleInput(std::uintptr_t two, std::uintptr_t one) noexcept
{
    const auto want = Field<std::uint8_t>(one + kBattleInput);
    const auto have = Field<std::uint8_t>(two + kBattleInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(two), std::uint8_t{0})
                               : platform::CallGuarded(g_suspendHudInput, std::uint8_t{0}, std::uint8_t{0});
    Line(std::format(L"player 2's battle input {} -> {} ({}){}: now {}", have, want,
                     want != 0 ? L"ResumeHudInput(0)" : L"SuspendHudInput(0, 0)", set ? L"" : L" FAULTED",
                     Field<std::uint8_t>(two + kBattleInput)));
}

/// Player 2's controller follows the game's battle HUD mode through its own setter (§4.2, §7).
void MirrorHudMode(std::uintptr_t two, std::uintptr_t one, std::uint8_t memberType) noexcept
{
    const auto mode = Field<std::uint8_t>(one + kHudMode);
    if (Field<std::uint8_t>(two + kHudMode) != mode)
    {
        if (mode != 0)
        {
            Put(two + kStoredLeaderType, memberType);  // no leader-change handler on player 1's leader
        }
        // The game's battle start and end call only this setter (RE-p2-command-menu-lifecycle.md §4.1); a
        // full reset would also clear "setup done" (+0x777), which only Setup puts back.
        const bool set = platform::CallGuarded(g_hudMode, reinterpret_cast<void*>(two), mode);
        Line(std::format(L"player 2's controller battle HUD mode -> {}{}", mode, set ? L"" : L" (the setter FAULTED)"));
    }
    Put(two + kReadyB, Field<std::uint8_t>(one + kReadyB));  // every pass: the battle readiness stays live
    MirrorBattleInput(two, one);
}

struct Shielded
{
    std::int32_t mode = 0, other = 0;
    std::uint8_t input64 = 0;
    bool operator==(const Shielded&) const = default;
};

Shielded ReadShielded(std::uintptr_t slot50, std::uintptr_t input) noexcept
{
    return {Field<std::int32_t>(slot50 + kSlot50Mode), Field<std::int32_t>(slot50 + kSlot50Other),
            Field<std::uint8_t>(input + kInputByte64)};
}

void RestoreShielded(std::uintptr_t slot50, std::uintptr_t input, const Shielded& saved) noexcept
{
    if (ReadShielded(slot50, input) == saved)
    {
        return;
    }
    ++g_shieldHits;
    Put(slot50 + kSlot50Mode, saved.mode);
    Put(slot50 + kSlot50Other, saved.other);
    Put(input + kInputByte64, saved.input64);
    if (g_shieldHits <= 5 || g_shieldHits % 600 == 0)
    {
        Line(std::format(L"player 2's tick changed a shared UI value; put back ({} time(s))", g_shieldHits));
    }
}

/// A controller's menu widget, or null.
void* MenuOf(std::uintptr_t controller) noexcept
{
    const auto weak   = Field<std::uint64_t>(controller + kMenuWidget);
    void*      widget = nullptr;
    return weak != 0 && platform::CallGuarded(g_weakGet, widget, &weak) ? widget : nullptr;
}

/// Player 2's command menu takes player 1's visibility through the game's setter (in battle; Setup
/// shows it only while a controller's battle input is live at Setup time). The field menu (party list)
/// is shown by the controller's own show timer once its input flag is on (MirrorPartyListInput).
void MirrorVisibility(std::uintptr_t two, std::uintptr_t one) noexcept
{
    for (const std::uint32_t offset : {kMenuWidget})
    {
        const auto oneWidget = reinterpret_cast<std::uintptr_t>(MenuOf(one + offset - kMenuWidget));
        const auto twoWidget = reinterpret_cast<std::uintptr_t>(MenuOf(two + offset - kMenuWidget));
        if (oneWidget == 0 || twoWidget == 0)
        {
            continue;
        }
        const auto want = Field<std::uint8_t>(oneWidget + kVisibility);
        const auto have = Field<std::uint8_t>(twoWidget + kVisibility);
        if (want != have)
        {
            const bool set = platform::CallGuarded(g_setVisibility, reinterpret_cast<void*>(twoWidget), want);
            if (++g_visibilitySets > 3 && g_visibilitySets % 600 != 0 && set)
            {
                continue;
            }
            Line(std::format(L"({} copies so far) ", g_visibilitySets) + std::format(L"player 2's {} visibility {} -> {}{}", offset == kMenuWidget ? L"command menu" : L"field menu",
                             have, want, set ? L"" : L" (FAULTED)"));
        }
    }
}

/// Player 2's party list input flag follows player 1's, through the game's own calls (§10.2 R1).
void MirrorPartyListInput(std::uintptr_t two, std::uintptr_t one) noexcept
{
    const auto want = Field<std::uint8_t>(one + kPartyListInput);
    const auto have = Field<std::uint8_t>(two + kPartyListInput);
    if (want == have)
    {
        return;
    }
    const bool set = want != 0 ? platform::CallGuarded(g_resumeHudInput, reinterpret_cast<void*>(two), std::uint8_t{1})
                               : platform::CallGuarded(g_partyListInput, reinterpret_cast<void*>(two + kPartyList),
                                                       std::uint8_t{0}, std::uint8_t{1});
    Line(std::format(L"player 2's party list input {} -> {} ({}){}", have, want,
                     want != 0 ? L"ResumeHudInput(1)" : L"SetInputEnabled(0)", set ? L"" : L" FAULTED"));
}

/// The battle system's command locks copied onto player 2's controller: it writes them into the
/// game's only (§10.2), and player 2's rows are all greyed while "commands enabled" is 0.
void CopyCommandLocks(std::uintptr_t two, std::uintptr_t one) noexcept
{
    static bool loggedFirst = false;
    if (!loggedFirst)
    {
        loggedFirst = true;
        Line(std::format(L"player 2's controller before the first lock copy: commands enabled {}, locked {}",
                         Field<std::uint8_t>(two + kCommandsEnabled), Field<std::uint8_t>(two + kCommandsLocked)));
    }
    for (const std::uint32_t offset : {kCommandsEnabled, kCommandsLocked})
    {
        const auto value = Field<std::uint8_t>(one + offset);
        if (Field<std::uint8_t>(two + offset) != value)
        {
            Put(two + offset, value);
            Line(std::format(L"player 2's commands {} -> {}", offset == kCommandsEnabled ? L"enabled" : L"locked", value));
        }
    }
    const auto oneLocks = Field<std::uintptr_t>(one + kCategoryLocks);
    const auto twoLocks = Field<std::uintptr_t>(two + kCategoryLocks);
    const auto count = std::min({Field<std::int32_t>(one + kCategoryCount), Field<std::int32_t>(two + kCategoryCount),
                                 kMaxCategories});
    if (oneLocks == 0 || twoLocks == 0 || count <= 0)
    {
        return;
    }
    std::array<std::uint8_t, kMaxCategories> from{}, to{};
    if (platform::CopyGuarded(from.data(), reinterpret_cast<const void*>(oneLocks), static_cast<std::size_t>(count)) &&
        platform::CopyGuarded(to.data(), reinterpret_cast<const void*>(twoLocks), static_cast<std::size_t>(count)) &&
        from != to)
    {
        platform::WriteGuarded(reinterpret_cast<void*>(twoLocks), from.data(), static_cast<std::size_t>(count));
        std::wstring bytes;
        for (std::int32_t i = 0; i < count; ++i)
        {
            bytes += std::format(L" {}", from[static_cast<std::size_t>(i)]);
        }
        Line(std::format(L"player 2's command category locks ->{}", bytes));
    }
}

/// Player 2's menu keeps the same bottom focus layer as player 1's (a battle event or help menu pops
/// player 1's: player 2's follows), through the controller's own setter. The mirrored Resume/Suspend
/// (MirrorBattleInput) already keep it in step, so this acts only on a change they did not carry.
void MirrorMenuLayer(std::uintptr_t two, std::uintptr_t one) noexcept
{
    const auto want = Field<std::uint8_t>(one + kMenuLayer);
    if (want == Field<std::uint8_t>(two + kMenuLayer))
    {
        return;
    }
    const bool set = platform::CallGuarded(g_setMenuLayer, reinterpret_cast<void*>(two), want);
    Line(std::format(L"player 2's menu focus layer -> {}{}", want, set ? L"" : L" (FAULTED)"));
}

/// What the log shows of player 2's controller, as raw values.
struct Snapshot
{
    std::array<std::int32_t, kLoggedRequests.size()> requests{};
    std::array<std::uint8_t, 7>  bytes{};  // 540 541 5E1 782 25C 258 774
    std::array<std::uint8_t, kLoggedGates.size()> gates{};
    std::int32_t guardLatch = 0, waitFilter = 0;
    std::uint8_t storedType = 0, pinch = 0;
    std::array<std::int32_t, 2> members{};
    // Beside player 1's: 777, 541, 25C, 783, 258, 5E1 (u8) and the pending open (i32), each [0] player 1, [1] player 2.
    std::array<std::array<std::uint8_t, 8>, 2> compared{};
    std::uint8_t                               fieldMenuVisibility = 0xFF;  // player 2's (0 Visible, 1 Collapsed)
    bool                                       leaderIsMember = false;       // inside the pass
    std::array<std::int32_t, 2>                openPending{};
    std::int32_t                               path = 0;  // player 2's live focus path length (inside the pass)
    bool operator==(const Snapshot&) const = default;
};
Snapshot g_logged{};
bool     g_loggedOnce = false;

void Watch(std::uintptr_t two, std::uintptr_t one) noexcept
{
    if (g_watchLines >= kMaxWatchLines)
    {
        return;
    }
    Snapshot now;
    constexpr std::array<std::uint32_t, 8> kCompared{kGate777, kBlocked, kBattleInput, kMenuLayer, kSwitchingLayer, kReady,
                                                     kPartyListInput, kSetupPending};
    for (std::size_t side = 0; side < 2; ++side)
    {
        const auto ctrl = side == 0 ? one : two;
        for (std::size_t i = 0; i < kCompared.size(); ++i)
        {
            now.compared[side][i] = Field<std::uint8_t>(ctrl + kCompared[i]);
        }
        now.openPending[side] = Field<std::int32_t>(ctrl + kOpenPending);
    }
    now.path = focus_scope::LivePathCount();
    if (const auto fieldMenu = reinterpret_cast<std::uintptr_t>(MenuOf(two + kFieldMenu - kMenuWidget)); fieldMenu != 0)
    {
        now.fieldMenuVisibility = Field<std::uint8_t>(fieldMenu + kVisibility);
    }
    void* leader = nullptr;
    now.leaderIsMember = platform::CallGuarded(g_resolveLeader, leader) && leader != nullptr && leader == generation::Member();
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        now.requests[i] = RequestOf(two, kLoggedRequests[i]);
    }
    constexpr std::array<std::uint32_t, 7> kBytes{kHudMode, kBlocked, kReady, kReadyB, kBattleInput, kSwitchingLayer,
                                                  kHudSoundMode};
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = Field<std::uint8_t>(two + kBytes[i]);
    }
    for (std::size_t i = 0; i < kLoggedGates.size(); ++i)
    {
        now.gates[i] = Field<std::uint8_t>(two + kLoggedGates[i]);
    }
    now.guardLatch = Field<std::int32_t>(two + kGuardLatch);
    now.waitFilter = Field<std::int32_t>(two + kWaitFilter);
    now.storedType = Field<std::uint8_t>(two + kStoredLeaderType);
    now.pinch      = Field<std::uint8_t>(two + kPinch);
    now.members    = {Field<std::int32_t>(two + kMembers), Field<std::int32_t>(two + kMembers + 4)};
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_logged     = now;
    g_loggedOnce = true;
    std::wstring requests;
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        requests += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
    }
    const auto& b = now.bytes;
    const auto& g = now.gates;
    const auto  side = [&now](std::size_t i) { return std::format(L"{}/{}", now.compared[0][i], now.compared[1][i]); };
    WatchLine(std::format(L"P1/P2: 777 {} 541 {} 25C {} 783 {} 258 {} 5E1 {} 4A0 {} 775 {} open {}/{} | P2 path {}, field "
                          L"menu visibility {}, leader is P2's member {}",
                          side(0), side(1), side(2), side(3), side(4), side(5), side(6), side(7), now.openPending[0],
                          now.openPending[1], now.path, now.fieldMenuVisibility, now.leaderIsMember ? 1 : 0));
    WatchLine(std::format(L"player 2's controller: req{} | guard latch {} | 540={} 541={} 5E1={} 782={} 25C={} 258={} 774={} | "
                     L"type {} leader {} ally {} | wait {} pinch {} | 776={} 77C={} 77F={} 787={} | slot 0: P1 {:.3f} "
                     L"P2 {:.3f} written {:.3f}",
                     requests, now.guardLatch, b[0], b[1], b[2], b[3], b[4], b[5], b[6], now.storedType, now.members[0],
                     now.members[1], now.waitFilter, now.pinch, g[0], g[1], g[2], g[3], g_oneSlot, g_twoSlot, g_written));
}

void Refused(const wchar_t* why) noexcept
{
    ++g_refusals;
    if (g_refusals <= 5 || g_refusals % 600 == 0)
    {
        Line(std::format(L"player 2's pass skipped: {} ({} time(s))", why, g_refusals));
    }
}

void ReleaseTint(const wchar_t* why) noexcept;  // below, with the tint's thunks

/// Player 2's controller's tick, inside player 2's scopes (§13.2).
void PlayerTwoPass(float deltaTime) noexcept
{
    const auto two    = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto one    = Field<std::uintptr_t>(g_endUISystem + kController);
    void*      member = generation::Member();
    if (two == 0 || one == 0 || one == two || member == nullptr ||
        (Field<std::uint8_t>(one + kHudMode) == 0 && Field<std::uint8_t>(two + kHudMode) == 0))
    {
        ReleaseTint(L"released: player 2's menu is not ticked (out of battle)");
        return;  // out of battle: player 2's controller has nothing to do yet
    }
    const auto memberType = party_member::PlayerTypeOf(member);
    const auto memberWeak = leader_scope::WeakOf(member);
    const auto manager    = Field<std::uintptr_t>(g_partyManager);
    if (!memberType || memberWeak == 0 || manager == 0)
    {
        return Refused(L"player 2's member could not be read");
    }
    const auto before = std::array<std::uint64_t, 3>{Field<std::uint64_t>(manager + kPrimary),
                                                     Field<std::uint64_t>(manager + kLeaderWeak),
                                                     Field<std::uint8_t>(manager + kLeaderType)};
    const auto leaderType = static_cast<std::uint8_t>(before[2]);
    {
        focus_scope::FocusScope focus(reinterpret_cast<void*>(two));
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
        const pad_buttons::ButtonScope   buttons;
        const lock_on::TargetScope       targets;
        const leader_scope::LeaderScope  leader(memberWeak);
        const leader_scope::LeaderTypeScope type(*memberType);
        if (!buttons.Held() || !leader.Held() || !type.Held())
        {
            return Refused(L"pad 2's buttons or the party leader");
        }
        const command_rules::PlayerTwoRules rules(leaderType);
        g_pass = 2;
        ++g_passes;
        // Shielded from here: the mirrors call the game's setters on player 2's controller too.
        const auto     slot50 = Field<std::uintptr_t>(g_endUISystem + kSlot50);
        const auto     input  = Field<std::uintptr_t>(g_endUISystem + kInput);
        const Shielded saved  = ReadShielded(slot50, input);
        MirrorHudMode(two, one, *memberType);
        MirrorVisibility(two, one);
        MirrorPartyListInput(two, one);
        MirrorMenuLayer(two, one);
        CopyCommandLocks(two, one);
        WriteRequests(two, one);
        Put(two + kHudSoundMode, Field<std::uint8_t>(two + kHudMode));  // no second battle-HUD sound
        const bool     ticked = platform::CallGuarded(reinterpret_cast<TickFn>(g_tickCall.Original()),
                                                      reinterpret_cast<void*>(two), deltaTime);
        RestoreShielded(slot50, input, saved);
        g_pass   = 0;
        g_passed = ticked;
        if (g_twoHoldsTint && Field<std::int32_t>(two + kWaitFilter) == 0)
        {
            ReleaseTint(L"WARNING: player 2's menu is idle but held a count; released");
        }
        if (!ticked)
        {
            focus.Abandon();
            ReleaseTint(L"released: player 2's tick faulted");
            g_installed = false;
            Line(L"FAILED: player 2's controller's tick faulted; player 2's command controller stops, and with it "
                 L"player 2's attack, guard and dodge");
        }
        Watch(two, one);
    }
    const auto after = std::array<std::uint64_t, 3>{Field<std::uint64_t>(manager + kPrimary),
                                                    Field<std::uint64_t>(manager + kLeaderWeak),
                                                    Field<std::uint8_t>(manager + kLeaderType)};
    if (after != before)
    {
        Line(L"WARNING: the primary controller or the party leader differs after player 2's pass");
    }
}

/// Which of the actions the log counts on its own a key is (0 CommandMenu, 1 Attack, 2 Unique, 3 Guard,
/// 4 Dodge), or -1.
int KnownAction(std::uint64_t key) noexcept
{
    const std::array<std::uintptr_t, 5> keys{g_commandMenuKey, g_keys[kAttack], g_keys[kUnique], g_keys[kGuard],
                                             g_keys[kDodge]};
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i] != 0 && Field<std::uint64_t>(keys[i]) == key)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// A name for the log: the action keys it knows, else the raw key name.
std::wstring KeyText(std::uint64_t key)
{
    const std::array<std::pair<std::uintptr_t, const wchar_t*>, 5> known{
        {{g_commandMenuKey, L"CommandMenu"}, {g_keys[kAttack], L"Attack"}, {g_keys[kUnique], L"UniqueAbility"},
         {g_keys[kGuard], L"Guard"}, {g_keys[kDodge], L"Dodge"}}};
    for (const auto& [address, name] : known)
    {
        if (address != 0 && Field<std::uint64_t>(address) == key)
        {
            return name;
        }
    }
    return std::format(L"key {:#x}", key);
}

/// Runs `call` (a guarded game call, true when it did not fault) in player 2's UI: player 2's focus
/// context and scopes, in battle, with player 2's menu layer in place and player 1's top layer the
/// command menu's own (a pause menu or popup keeps pad 2 out of both menus). False, running nothing,
/// when those do not hold; true once it ran (a fault stops the route for the session).
template <class Call>
bool InPlayerTwoUi(const wchar_t* what, Call&& call) noexcept
{
    if (!g_installed || g_routeFailed || g_pass != 0 || !generation::OnGameThread() || !generation::Active())
    {
        return false;
    }
    const auto two    = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto one    = Field<std::uintptr_t>(g_endUISystem + kController);
    void*      member = generation::Member();
    if (two == 0 || one == 0 || one == two || member == nullptr || Field<std::uint8_t>(two + kHudMode) == 0 ||
        Field<std::uint8_t>(two + kMenuLayer) == 0 || Field<std::uint8_t>(one + kBattleInput) == 0 ||
        Field<std::uint8_t>(one + kMenuLayer) == 0 || !focus_scope::PlayerOneTopIsCommandMenu())
    {
        return false;
    }
    const auto memberType = party_member::PlayerTypeOf(member);
    const auto memberWeak = leader_scope::WeakOf(member);
    const auto manager    = Field<std::uintptr_t>(g_partyManager);
    if (!memberType || memberWeak == 0 || manager == 0)
    {
        return false;
    }
    const auto leaderType = Field<std::uint8_t>(manager + kLeaderType);
    focus_scope::FocusScope focus(reinterpret_cast<void*>(two));
    if (!focus.Entered())
    {
        return false;
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        return false;
    }
    const lock_on::TargetScope      targets;
    const leader_scope::LeaderScope leader(memberWeak);
    const leader_scope::LeaderTypeScope type(*memberType);
    if (!leader.Held() || !type.Held())
    {
        return false;
    }
    const command_rules::PlayerTwoRules rules(leaderType);
    g_pass         = 3;
    const bool ran = call();
    g_pass         = 0;
    if (!ran)
    {
        focus.Abandon();
        g_routeFailed = true;
        Line(std::format(L"FAILED: pad 2's {} faulted in player 2's menu; pad 2 stays out of the menus", what));
    }
    return true;
}

/// For the route log: player 2's battle input, its action requests, and player 1's capture map's count.
std::wstring RouteState(std::uintptr_t two)
{
    const auto input = Field<std::uintptr_t>(g_endUISystem + kInput);
    return std::format(L"25C {} req 6:{} 12:{} 13:{} 26:{} 32:{} 37:{} | captures {}", Field<std::uint8_t>(two + kBattleInput),
                       RequestOf(two, 6), RequestOf(two, kReqAttack), RequestOf(two, kReqUnique), RequestOf(two, 26),
                       RequestOf(two, kReqGuardDodge), RequestOf(two, kReqGuard),
                       Field<std::int32_t>(input + 0xB0) - Field<std::int32_t>(input + 0xDC));
}

/// Pad 2's press through the game's own dispatcher in player 2's UI, so its route, list and
/// command-menu handler act on player 2's menu (RE-p2-command-menu-input.md §3.3, §10 Stage 3d).
/// Target switching stays with player_two/lock_on.
bool RoutePadTwo(pad_buttons::ActionDownFn dispatch, void* owner, void* unused, std::uint32_t sourceBit, void* event,
                 std::uint8_t alreadyHandled, char& result) noexcept
{
    const auto key = Field<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event) + kEventKey);
    if (key == 0 || key == Field<std::uint64_t>(g_prevTargetKey) || key == Field<std::uint64_t>(g_nextTargetKey))
    {
        return false;
    }
    char taken = 1;  // after a fault: not dispatched again in player 1's context
    const int  action = KnownAction(key);
    const auto two    = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    std::wstring before;
    const bool routed = InPlayerTwoUi(L"press", [&] {
        const bool logIt = action > 0 && g_routedKnown[static_cast<std::size_t>(action)] < 10;
        if (logIt)
        {
            before = RouteState(two);
        }
        const bool ran = platform::CallGuarded(dispatch, taken, owner, unused, sourceBit, event, alreadyHandled);
        if (logIt)
        {
            Line(std::format(L"pad 2's {} in player 2's UI: before [{}] after [{}]", KeyText(key), before, RouteState(two)));
        }
        return ran;
    });
    if (!routed)
    {
        return false;
    }
    result = taken;
    if (action >= 0 ? ++g_routedKnown[static_cast<std::size_t>(action)] <= 10 : (++g_routed <= 20 || g_routed % 500 == 0))
    {
        Line(std::format(L"pad 2's {} went to player 2's menu (taken: {}; {} press(es))", KeyText(key), taken != 0 ? 1 : 0,
                         g_routed));
    }
    return true;
}

/// A pad-2 release that did not go through player 2's UI (player 1's pause menu or a popup was up):
/// its action's held request on player 2's controller cleared anyway, as the up handler would.
void ClearHeldForRelease(std::uint64_t key) noexcept
{
    const auto two = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    if (two == 0 || key == 0)
    {
        return;
    }
    if (key == Field<std::uint64_t>(g_keys[kGuard]))
    {
        SetRequest(two, kReqGuard, 0);
        SetRequest(two, kReqGuardDodge, 0);
        SetRequest(two, kReqSynergyPane, 0);
    }
    else if (key == Field<std::uint64_t>(g_keys[kAttack]))
    {
        SetRequest(two, kReqAttack, 0);
    }
    else if (key == Field<std::uint64_t>(g_keys[kUnique]))
    {
        SetRequest(two, kReqUnique, 0);
    }
}

/// Replaces the input processor's key-up handler: notes which pad the release came from.
std::uint8_t KeyUp(void* self, void* application, const void* keyEvent) noexcept
{
    const std::int32_t previous = g_upPad;
    g_upPad = Field<std::int32_t>(reinterpret_cast<std::uintptr_t>(keyEvent) + kKeyEventUser);
    const auto answer = reinterpret_cast<KeyUpFn>(g_keyUpSlot.Original())(self, application, keyEvent);
    g_upPad = previous;
    return answer;
}

/// Replaces the up route's call: pad 2's releases walk player 2's focus path, so its menu clears its
/// own held requests (Guard, Attack, the synergy pane) as the game's up handler does.
std::uint8_t UpRoute(void* owner, void* event) noexcept
{
    const auto original = reinterpret_cast<UpRouteFn>(g_upRouteCall.Original());
    std::uint8_t answer = 0;
    if (g_upPad >= 0 && g_upPad == pad_sticks::ControllerIdOf(generation::PlayerTwoController()) &&
        InPlayerTwoUi(L"release", [&] { return platform::CallGuarded(original, answer, owner, event); }))
    {
        ++g_upRouted;
        return answer;
    }
    const std::uint8_t result = original(owner, event);
    if (g_upPad >= 0 && g_upPad == pad_sticks::ControllerIdOf(generation::PlayerTwoController()))
    {
        ClearHeldForRelease(Field<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event) + kEventKey));
    }
    return result;
}
/// The tint's filter manager and, for the log, its latch and count for the tint type.
std::uintptr_t FilterManager() noexcept
{
    return Field<std::uintptr_t>(g_endUISystem + kFilterManager);
}

void TintLine(const wchar_t* what) noexcept
{
    if (++g_tintLines > 60)
    {
        return;
    }
    const auto fm  = FilterManager();
    const auto one = Field<std::uintptr_t>(g_endUISystem + kController);
    const auto two = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    platform::Log(L"command menu tick: tint {} (pass {}; wait P1 {} P2 {}; latch {} count {}; P2 holds {})", what, g_pass,
                  one != 0 ? Field<std::int32_t>(one + kWaitFilter) : -1, two != 0 ? Field<std::int32_t>(two + kWaitFilter) : -1,
                  Field<std::uint8_t>(fm + kFilterLatch + kTintType), Field<std::int32_t>(fm + kFilterCount + 4 * kTintType),
                  g_twoHoldsTint ? 1 : 0);
}

/// Player 2's count of the tint given back (its menu closed, or it stops holding for another reason).
void ReleaseTint(const wchar_t* why) noexcept
{
    if (!g_twoHoldsTint)
    {
        return;
    }
    const auto fm = FilterManager();
    if (fm != 0)
    {
        platform::CallGuarded(g_refDec, reinterpret_cast<void*>(fm), kTintType);
    }
    g_twoHoldsTint = false;
    TintLine(why);
}

/// Replaces the wait mode's filter on: player 1's menu takes the latch as the game does; player 2's
/// takes a count of its own.
void TintOn(void* world, std::uint8_t type) noexcept
{
    if (g_pass != 2)
    {
        reinterpret_cast<FilterOnFn>(g_filterOnCall.Original())(world, type);
        return;
    }
    const auto fm = FilterManager();
    if (!g_twoHoldsTint && fm != 0 && platform::CallGuarded(g_refInc, reinterpret_cast<void*>(fm), type))
    {
        g_twoHoldsTint = true;
    }
    TintLine(L"on for player 2's menu (a count)");
}

/// Replaces the wait mode's filter off: player 2's gives back its count; player 1's drops the latch.
void TintOff(std::uint8_t type) noexcept
{
    if (g_pass != 2)
    {
        reinterpret_cast<FilterOffFn>(g_filterOffCall.Original())(type);
        TintLine(L"off for player 1's menu (the latch)");
        return;
    }
    ReleaseTint(L"off for player 2's menu (its count)");
}

/// Replaces the wait mode's sound off (event 13 and the audio mode cleared): skipped while the other
/// menu is still in wait mode. Player 1's ticks first, so if both close in one frame player 2's plays it.
void WaitSoundOff(std::uint8_t index) noexcept
{
    const auto one  = Field<std::uintptr_t>(g_endUISystem + kController);
    const auto two  = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto wait = [](std::uintptr_t ctrl) { return ctrl != 0 ? Field<std::int32_t>(ctrl + kWaitFilter) : 0; };
    const auto s1 = wait(one), s2 = wait(two);
    if ((g_pass == 1 && (s2 == 1 || s2 == 5 || s2 == 4)) || (g_pass == 2 && (s1 == 1 || s1 == 5)))
    {
        TintLine(L"wait sound off skipped (the other menu is still in wait mode)");
        return;
    }
    reinterpret_cast<MenuSoundFn>(g_soundOffCall.Original())(index);
}

/// Replaces the game's controller tick call: the game's controller, then player 2's.
void Tick(void* controller, float deltaTime) noexcept
{
    ++g_frame;
    g_pass   = 1;
    reinterpret_cast<TickFn>(g_tickCall.Original())(controller, deltaTime);
    g_pass   = 0;
    g_passed = false;
    if (g_installed && generation::OnGameThread() && generation::Active())
    {
        PlayerTwoPass(deltaTime);
    }
    else
    {
        ReleaseTint(L"released: player 2's menu is not ticked (player 2 is out)");
    }
}

/// Replaces the world-slow write of every controller tick: either menu slows the world (policy A).
void Slot(std::uint8_t slot, float value) noexcept
{
    const auto original = reinterpret_cast<SlotFn>(g_slotCall.Original());
    if (g_pass == 2)
    {
        g_twoSlot = value;
        if (g_oneFrame != g_frame)
        {
            Line(L"WARNING: player 2's slow value came without player 1's this frame; written as is");
        }
        g_written = g_oneFrame == g_frame ? std::min(g_oneSlot, value) : value;
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
    if (used != g_snapshotEntries)
    {
        g_snapshotEntries = used;
        Line(std::format(L"player 2's command snapshot (not written): {} type(s) with a command", used));
    }
}

std::uint8_t GuideShow(void* guide, std::uint8_t mode) noexcept
{
    // With player 2's own key guide in the slot (player_two/focus_scope), player 2's controller drives it
    // natively; without one, player 2's pass leaves the shared guide alone.
    if (g_pass == 2 && command_menu_two::KeyGuide() == nullptr)
    {
        ++g_guideSkips;
        Line(std::format(L"player 2's key-guide mode {} not shown ({} time(s))", mode, g_guideSkips));
        return 0;
    }
    return reinterpret_cast<GuideShowFn>(g_guideShowCall.Original())(guide, mode);
}

void GuideMode8() noexcept
{
    if (g_pass == 2 && command_menu_two::KeyGuide() == nullptr)
    {
        ++g_guideSkips;
        Line(std::format(L"player 2's key-guide mode 8 not set ({} time(s))", g_guideSkips));
        return;
    }
    reinterpret_cast<GuideMode8Fn>(g_guideMode8Call.Original())();
}

}  // namespace

/// Pad 2's releases to player 2's menu: the key-up slot wrapped (the pad of each release noted) and the
/// up route's call redirected. Optional: without it pad 2's releases keep running player 1's up route.
void InstallReleases(const platform::GameImage& image) noexcept
{
    const auto upRoute = platform::FindFunction(image, kUpRoute);
    const auto entry   = image.Rebase(kKeyUpEntry, sizeof(void*));
    const auto keyUp   = image.Rebase(kKeyUp);
    const auto site    = image.Rebase(kUpRouteCall, 5);
    const bool ok      = upRoute && entry && keyUp && site && image.BytesMatch(kKeyUpAfterFirst, kKeyUpRestBytes) &&
                    g_keyUpSlot.Replace(*entry, *keyUp, reinterpret_cast<std::uintptr_t>(&KeyUp)) &&
                    g_upRouteCall.Install(*site, *upRoute, reinterpret_cast<std::uintptr_t>(&UpRoute));
    if (!ok && g_keyUpSlot.Original() != 0)
    {
        g_keyUpSlot.Restore();
    }
    platform::Log(L"command menu tick: pad 2's releases {}", ok ? L"reach player 2's menu in battle"
                                                                 : L"UNAVAILABLE (a check failed): they keep player 1's up route");
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto tick = find(kTick), slot = find(kSlot), snapshot = find(kSnapshot), show = find(kGuideShow),
               mode8 = find(kGuideMode8), hud = find(kSetHudMode), resume = find(kResumeHudInput), suspend = find(kSuspendHudInput), listInput = find(kSetPartyListInput),
               leader = find(facts::kResolvePartyLeader), layer = find(kSetMenuLayer), visibility = find(kSetVisibility),
               weakGet = find(facts::kWeakObjectGet), filterOn = find(kFilterOn), filterOff = find(kFilterOff),
               menuSound = find(kMenuSound), refInc = find(kRefInc), refDec = find(kRefDec);
    const auto prevTarget = image.Rebase(kPrevTargetKey, 24), nextTarget = image.Rebase(kNextTargetKey, 24),
               commandMenu = image.Rebase(kCommandMenuKey, 24);
    const auto system  = image.Rebase(kEndUISystem, kSlot50 + sizeof(void*));
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    bool       ok      = tick && slot && snapshot && show && mode8 && hud && resume && suspend && listInput && leader && layer && visibility && weakGet && filterOn && filterOff && menuSound && refInc && refDec && prevTarget &&
                  nextTarget && commandMenu && system && manager;
    for (std::size_t i = 0; ok && i < kActions; ++i)
    {
        const auto key = image.Rebase(kActionKeys[i], 24);
        g_keys[i]      = key ? *key : 0;
        ok             = key.has_value();
    }
    if (ok)
    {
        g_hudMode      = reinterpret_cast<HudModeFn>(*hud);
        g_resumeHudInput  = reinterpret_cast<ResumeHudInputFn>(*resume);
        g_suspendHudInput = reinterpret_cast<SuspendHudInputFn>(*suspend);
        g_partyListInput = reinterpret_cast<PartyListInputFn>(*listInput);
        g_resolveLeader  = reinterpret_cast<LeaderFn>(*leader);
        g_endUISystem  = *system;
        g_partyManager = *manager;
        g_setMenuLayer   = reinterpret_cast<SetMenuLayerFn>(*layer);
        g_setVisibility  = reinterpret_cast<SetVisibilityFn>(*visibility);
        g_weakGet        = reinterpret_cast<WeakGetFn>(*weakGet);
        g_refInc         = reinterpret_cast<FilterCountFn>(*refInc);
        g_refDec         = reinterpret_cast<FilterCountFn>(*refDec);
        g_prevTargetKey  = *prevTarget;
        g_nextTargetKey  = *nextTarget;
        g_commandMenuKey = *commandMenu;
    }
    // The shields first, the tick last: no pass runs before its shields are in place.
    const auto redirect = [&image](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && call.Install(*live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    ok = ok && redirect(g_filterOnCall, kFilterOnCall, *filterOn, &TintOn) &&
         redirect(g_filterOffCall, kFilterOffCall, *filterOff, &TintOff) &&
         redirect(g_soundOffCall, kSoundOffCall, *menuSound, &WaitSoundOff);
    ok = ok && redirect(g_slotCall, kSlotCall, *slot, &Slot) && redirect(g_snapshotCall, kSnapshotCall, *snapshot, &CommandSnapshot) &&
         redirect(g_guideShowCall, kGuideShowCall, *show, &GuideShow) &&
         redirect(g_guideMode8Call, kGuideMode8Call, *mode8, &GuideMode8) &&
         redirect(g_tickCall, kTickCall, *tick, &Tick);
    g_installed = ok;
    if (!ok)
    {
        Remove();
    }
    else
    {
        pad_buttons::SetPlayerTwoRoute(&RoutePadTwo);  // pad 2's presses to player 2's menu in battle
        InstallReleases(image);                        // optional: pad 2's releases to player 2's menu
    }
    platform::Log(L"command menu tick: {}", ok ? L"player 2's command controller ticks in battle and takes player 2's "
                                                 L"attack, guard and dodge; pad 2's presses reach player 2's menu in battle (logged)"
                                               : L"UNAVAILABLE (a check failed): player 2 has no attack, guard or dodge");
    return ok;
}

void Remove() noexcept
{
    pad_buttons::SetPlayerTwoRoute(nullptr);
    ReleaseTint(L"released: the stage is removed");
    g_installed = false;
    if (g_keyUpSlot.Original() != 0 && !g_keyUpSlot.Restore())
    {
        platform::Log(L"command menu tick: the key-up slot was changed after us; left in place (it passes through)");
    }
    for (auto* call : {&g_tickCall, &g_guideMode8Call, &g_guideShowCall, &g_snapshotCall, &g_slotCall, &g_upRouteCall,
                       &g_filterOnCall, &g_filterOffCall, &g_soundOffCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::command_menu_tick
