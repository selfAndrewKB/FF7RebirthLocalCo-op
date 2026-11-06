#include "world/field_pickup.h"

#include "facts/command_controller.h"
#include "facts/controller.h"
#include "facts/field_action.h"
#include "facts/hud.h"
#include "facts/world.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/tset_find.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"
#include "world/shop_talk.h"
#include "world/vending.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <intrin.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::world::field_pickup {

namespace seat_ladder = scopes::seat_ladder;

namespace {

using Clock = std::chrono::steady_clock;

// The objects (RE-p2-field-interact.md §1.1, §12; this build).
using facts::kEndGameRoot;
constexpr std::uint32_t  kRootStage         = 0x70;         // u8: the input step's FieldAction part runs from 8
using facts::kRootGameState;
using facts::kRootModeHolder;
constexpr std::uint32_t  kRootSlotHeldOwner = 0x210;        // -> +8: a u8 TickSlots writes (§12.1)
constexpr std::uint32_t  kSlotHeldByte      = 0x08;
using facts::kRootWorldItems;
using facts::kCmdOpenPending;
using facts::kPauseMenuKey;  // InputKey24 "PauseMenu" (the poll needs it not held)

// The input step's gate fields in GS (§12.12 item 2): i32 each.
constexpr std::uint32_t kGsBlockA = 0x1A12288;
constexpr std::uint32_t kGsBlockB = 0x1A17548;
constexpr std::uint32_t kGsAllowA = 0x1A12744;  // + kGsAllowB: a non-zero sum polls despite the blocks
constexpr std::uint32_t kGsAllowB = 0x1A12494;
constexpr std::uint32_t kGsSlot0A = 0x1A12860;  // + kGsSlot0B: non-zero polls slot 0 only (and despite the blocks)
constexpr std::uint32_t kGsSlot0B = 0x1A125B0;
constexpr std::uint8_t  kGameFlagBlocksPoll = 11;
constexpr std::uint8_t  kModeBlocksPoll     = 2;

// FieldActionMgr (§12.9): the per-player ranges, exchanged with player 2's copy around the pass.
struct Range
{
    std::uint32_t offset;
    std::uint32_t size;
};
constexpr std::array<Range, 5> kRanges{{
    {0x90, 0x2F8 - 0x90},          // candidates, the show-all timer, the trace-handle map
    {0x48328, 0x48748 - 0x48328},  // labels, rows, slot entries, records, keys, locations, latches
    {0x4874C, 4},                  // the far-marker sweep timer
    {0x487A0, 0x487D8 - 0x487A0},  // the pending execution
    {0x487E8, 1},                  // re-executed from pending this frame
}};
constexpr std::size_t kCopySize = [] {
    std::size_t size = 0;
    for (const Range& range : kRanges)
    {
        size += range.size;
    }
    return size;
}();

constexpr std::uint32_t kCandidateMax    = 0x29C;    // i32: 64 (the inline storage)
constexpr std::uint32_t kTraceMapA       = 0x2D4;    // i32: 128 (as the ctor leaves it)
constexpr std::uint32_t kTraceMapB       = 0x2D8;    // i32: -1
using facts::kFieldActionSlotRows;
using facts::kFieldActionSlotTargets;
using facts::kFieldActionSlotHeld;
using facts::kFieldActionPending;
constexpr std::uint32_t kReExecuted      = 0x487E8;  // u8
constexpr std::uint32_t kFieldEnterFlag  = 0x487E9;  // u8, shared: the field-enter reset's own flag
using facts::kFieldActionPointList;
using facts::kFieldActionNodeNext;
using facts::kFieldActionNodePoint;
using facts::kFieldActionPointLock;
using facts::kFieldActionSlots;
using facts::kFieldActionMaxPoints;

// A point.
using facts::kFieldActionPointKey;
using facts::kFieldActionPointPreset;
using facts::kFieldActionPointFlags;
constexpr std::uint8_t  kTargetBits     = 0x06;   // bit 1 slot target, bit 2 runner-up: carried per player (§12.3)
constexpr std::uint8_t  kDisabledBit    = 0x10;   // the game's "point disabled": the candidate query skips it
constexpr std::uint32_t kPointMarker    = 0x78;   // UFieldTargetWidget*, the point's marker: carried per player (§12.15)

// The two maps whose points player 2's pass admits, both a stock TMap keyed by the point's FName at element +0
// (platform/tset_find.h): WorldItemMgr's items (§10.5, collectibles and materials) and TreasurePointMgr's chests
// (§13.1, the chest listener's own test). Where the map sits in its owner, and its element's shape.
struct MapShape
{
    std::uint32_t elements;  // from the owner object
    std::uint32_t stride;
    std::uint32_t hashNext;  // i32, in an element
};
constexpr MapShape      kWorldItemMap{facts::kWorldItemMapElements, facts::kWorldItemMapStride,
                                      facts::kWorldItemMapHashNext};
constexpr MapShape      kChestMap{0x10, 0x70, 0x60};
// TreasurePointMgr's second map, CharaPop key -> treasure key (24-byte elements; §14, the spawn handler's own lookup).
constexpr MapShape      kChestByPopKeyMap{0x60, 0x18, 0x10};
constexpr std::uint32_t kRootChests      = 0x298;  // TreasurePointMgr (not TreasureManager +0x280, the opened state)
constexpr int           kHashChecks     = 2000;  // lookups also checked against a walk of the map, then trusted

// The game's functions (§12.12), each checked by its first bytes.
constexpr std::uint8_t kTestFlagBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                           0x9C, 0x0A, 0x68, 0x01, 0x48, 0x2B, 0xE0, 0x0F};
constexpr std::uint8_t kModeClassBytes[] = {0x48, 0x8B, 0x05, 0x7D, 0x12, 0x94, 0x08, 0x48,
                                            0x8B, 0x88, 0xB8, 0x01, 0x00, 0x00, 0xB8, 0x02};
constexpr std::uint8_t kUIEntryBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xA2, 0x53,
                                          0x42, 0x01, 0x48, 0x2B, 0xE0, 0xE8, 0xAA, 0xCD};
constexpr std::uint8_t kIsHeldBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40,
                                         0x00, 0x00, 0x00, 0xE8, 0x44, 0x8B, 0x8A, 0x01};
constexpr std::uint8_t kPollSlot0Bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                            0x00, 0x00, 0x00, 0xE8, 0x60, 0x52, 0xC0, 0x00};
constexpr std::uint8_t kPoll8Bytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                        0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
constexpr std::uint8_t kLatchBytes[] = {0x48, 0x2B, 0xD1, 0x4C, 0x8D, 0x89, 0x38, 0x87,
                                        0x04, 0x00, 0x4C, 0x2B, 0xC1, 0x41, 0xBA, 0x08};
constexpr std::uint8_t kTickSlotsBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kUpdateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                         0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr std::uint8_t kFieldEnterBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                             0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54};
constexpr std::uint8_t kFieldExitBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr platform::FunctionFact kTestFlag{L"GameFlags3A8_TestBit", 0x140A2E488, kTestFlagBytes};
constexpr platform::FunctionFact kModeClass{L"Root1B8_GetModeClass_2BlocksFieldAction", 0x1407BDB64, kModeClassBytes};
constexpr platform::FunctionFact kUIEntry{L"EndUI23_IsEntry34ValuePositive", 0x140C89B84, kUIEntryBytes};
constexpr platform::FunctionFact kIsHeld{L"SquareInput_IsActionHeld_Global", 0x1408063DC, kIsHeldBytes};
using facts::kInputKeyCopy;
constexpr platform::FunctionFact kPollSlot0{L"FieldActionMgr_PollSlotButtons_Slot0Only", 0x1414A9CC0, kPollSlot0Bytes};
constexpr platform::FunctionFact kPoll8{L"FieldActionMgr_PollSlotButtons8", 0x1414A9D7C, kPoll8Bytes};
constexpr platform::FunctionFact kLatch{L"FieldActionMgr_LatchSlotButtons", 0x1414FA05C, kLatchBytes};
constexpr platform::FunctionFact kTickSlots{L"FieldActionMgr_TickSlots_ExecuteOnPress", 0x1414AA14C, kTickSlotsBytes};
constexpr platform::FunctionFact kUpdate{L"FieldActionMgr_Update_CandidatesSlotsRows", 0x140E7A700, kUpdateBytes};
constexpr platform::FunctionFact kFieldEnter{L"FieldActionMgr_OnFieldEnter_ResetSlots", 0x14161B4E0, kFieldEnterBytes};
constexpr platform::FunctionFact kFieldExit{L"FieldActionMgr_OnFieldExit_ClearCandidatesRows", 0x14161B408, kFieldExitBytes};
// Their one call site each, in AEndGameMode_FieldStepTick (§12.12 item 3).
constexpr std::uintptr_t kFieldEnterCall = 0x140D9C309;
constexpr std::uintptr_t kFieldExitCall  = 0x140D9FFD8;
// The visibility request (§12.13): its point-side preset's screen test projects the point through the world's first
// local player (the first controller in World->PlayerControllerList at World+0x1F0 whose player is local; RE-p2-hud.md
// reads it as the PlayerControllerList through GetFirstPlayerController, the field-interact doc names it
// ControllerList), always player 1's. Inside player 2's pass the request runs with player 2's controller first in that
// list. Entry: mov [rsp+18h],rbx (the 5 bytes replaced), then push rbp/rsi/rdi/r12/r13/r14/r15.
constexpr std::uint8_t   kRequestBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::FunctionFact kRequest{L"the world items' screen-visibility request", 0x140ADE688, kRequestBytes};
constexpr std::size_t    kRequestFirst   = 5;

// The pass's hooks (§10.A, §12.12): the game mode tick's call of the selection update (the pass runs right after it),
// WorldItemMgr's mark by key (a key player 2's pass marks is noted; its entry's first instruction, 5 bytes) and the
// WorldItem tick's call of the grant (run with player 2's controller as the primary one for a noted key).
constexpr std::uintptr_t kUpdateCall      = 0x1423EF160;
constexpr std::uint8_t   kMarkByKeyBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00};
// u8(wim, FName key by value)
constexpr platform::FunctionFact kMarkByKey{L"WorldItemMgr's mark by key", 0x140C10F94, kMarkByKeyBytes};
constexpr std::size_t    kMarkFirst       = 5;
constexpr std::uintptr_t kAcquireCall     = 0x140DBE165;
constexpr std::uintptr_t kAcquire         = 0x140C07A3C;  // (wim, entry); the caller ignores its result
using facts::kFieldActionEntryKeyBack;
using MarkByKeyFn = std::uint8_t (*)(void* items, std::uint64_t key);
using AcquireFn   = void (*)(void* items, void* entry);
// UObjectBase's outer, and UWorld's PlayerControllerList at +0x1F0 (RE-p2-hud.md, GetFirstPlayerController reads
// [0] there): TArray<TWeakObjectPtr<APlayerController>> (8-byte entries). Its order with three players is unsettled
// [ASSUMPTION: player 1's, then the seats' in join order].
constexpr std::uint32_t  kObjectOuter      = 0x20;
constexpr std::uint32_t  kWorldControllers = 0x1F0;
constexpr std::uint32_t  kWorldControllerCount = 0x1F8;
// The marker list (§12.14): EndUISystem+0x98 points to a TSparseArray of 140-byte elements, the marker's weak widget
// at +0x00. The game's destroy (§12.15), the call RemovePointByKey makes: it finds the element by the widget, releases
// its parts, pools the Slate widget and removes the element, at once. Entry: push rbx; mov eax,20h; call (stack probe).
constexpr std::uint32_t kMarkerEntry    = 140;
constexpr std::uint8_t  kDestroyBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                           0x28, 0xFD, 0xEE, 0x00, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kDestroy{L"FieldTargetMarker_Destroy", 0x1411BF1FC, kDestroyBytes};

using TestFlagFn  = std::uint8_t (*)(std::uint8_t bit);
using ModeClassFn = std::uint8_t (*)();
using UIEntryFn   = std::uint8_t (*)();
using IsHeldFn    = std::uint8_t (*)(void* key);  // releases the key's reference
using CopyKeyFn   = void (*)(void* to, const void* from);  // 24 bytes, one more reference
using PollFn      = void (*)(void* mgr, std::uint8_t* held, std::uint8_t* pressed);
using TickFn      = void (*)(void* mgr, float dt);
using ResetFn     = void (*)(void* mgr);
using RequestFn   = std::uint64_t (*)(void* system, void* record);  // the handle in eax
using DestroyFn   = void (*)(void* widget);

constexpr auto kStatusEvery = std::chrono::seconds(60);

/// Why a pass did not run (counted for the status line).
enum Skip : int
{
    kSkipScopes,  // a scope did not hold
    kSkipMember,  // player 2's member or its type could not be read
    kSkipCount
};

/// Why the mod hands a marker widget to the game's destroy (logged with it, WhyText).
enum Why : int
{
    kWhyPointLeft,      // the seat's: its point left the live list between passes
    kWhyRemovedInPass,  // player 1's: its point was removed during the seat's pass (the removal destroyed the seat's)
    kWhySecond,         // the seat's: a second widget for one key
    kWhyNoWeak,         // the seat's: no weak value could be made for it, so it could not be kept
    kWhyFieldExit,      // the seat's: the field-exit reset
    kWhyLeft,           // the seat's: the seat's player left (or a new manager)
    kWhyStopped,        // the seat's: the seat's pass stopped
};

/// "player 2's, its point left" and the like, for `seat`. May throw (the string).
std::wstring WhyText(Why why, int seat)
{
    const wchar_t* const who = players::SeatName(seat);
    switch (why)
    {
    case kWhyPointLeft:     return std::format(L"{}'s, its point left", who);
    case kWhyRemovedInPass: return std::format(L"player 1's, its point was removed during {}'s pass", who);
    case kWhySecond:        return std::format(L"{}'s, a second widget for one point", who);
    case kWhyNoWeak:        return std::format(L"{}'s, no weak value could be made for it", who);
    case kWhyFieldExit:     return std::format(L"{}'s, field exit", who);
    case kWhyLeft:          return std::format(L"{}'s, {} left", who, who);
    case kWhyStopped:       return std::format(L"{}'s, {}'s pass stopped", who, who);
    }
    return {};
}

std::uintptr_t g_root        = 0;  // live address of the root global
std::uintptr_t g_uiSystem    = 0;  // live address of the EndUISystem object
std::uintptr_t g_pauseKey    = 0;  // live address of the PauseMenu key
TestFlagFn     g_testFlag    = nullptr;
ModeClassFn    g_modeClass   = nullptr;
UIEntryFn      g_uiEntry     = nullptr;
IsHeldFn       g_isHeld      = nullptr;
CopyKeyFn      g_copyKey     = nullptr;
PollFn         g_pollSlot0   = nullptr;
PollFn         g_poll8       = nullptr;
PollFn         g_latch       = nullptr;
TickFn         g_tickSlots   = nullptr;
TickFn         g_update      = nullptr;
ResetFn        g_fieldEnter  = nullptr;
ResetFn        g_fieldExit   = nullptr;
DestroyFn      g_destroy     = nullptr;  // null: the markers stay shared (no per-player widgets)
platform::CallRedirect  g_updateCall;   // the pass's hooks
platform::EntryRedirect g_markByKey;
platform::CallRedirect  g_acquireCall;
HookObserver            g_observer;     // set before any field exists; read by the hooks on any thread
platform::CallRedirect g_enterCall;
platform::CallRedirect g_exitCall;
platform::EntryRedirect g_request;
bool           g_ready       = false;

using Kind = platform::ByteContext::Kind;

// Game thread.
struct Saved
{
    std::uint8_t* point;
    std::uint64_t key;       // the point's key then: a pool node reused during the pass is another point
    std::uint8_t  oneBits;   // player 1's target bits
    bool          disabled;  // the pass set the disabled bit
    std::uintptr_t oneMarker;  // player 1's marker widget
    bool          matched;   // found again after the pass (same node, same key)
};
struct TwoMarker
{
    std::uintptr_t widget;
    std::uint64_t  weak;    // its FWeakObjectPtr value: put back in only while it still names the widget
    bool           placed;  // put back in a point this pass
};
struct Doomed
{
    std::uintptr_t widget;
    std::uint64_t  weak;  // 0: not checked: a widget read from a point in this pass and held only by the mod since,
                          // so nothing can have freed and reused its address before DestroyDoomed (same frame)
    std::uint64_t  key;
    Why            why;
    int            seat;  // whose pass or copy it came from (the log)
};

// The status line's counts since the last line, per seat.
struct Counts
{
    std::uint32_t passes = 0, polled = 0, filtered = 0, worldItems = 0, chests = 0, vending = 0, shopkeepers = 0, executed = 0,
                  fieldResets = 0;
    std::uint32_t requests = 0, requestsAsTwo = 0;  // visibility requests in the pass; those with the seat first
    std::uint32_t markersMade = 0, markersGone = 0, markersDestroyed = 0;  // the seat's made; gone by the game; the mod's
    std::array<std::uint32_t, kSkipCount> skips{};
    bool operator==(const Counts&) const = default;
};

/// One seat's pickup record: its copy of the manager's five ranges, the targets and marker widgets it carries by point
/// key, the WorldItem keys its pass marked, and its counts. By seat (players/seats.h; index 0, player 1's, unused:
/// player 1's state is the manager's own). Game thread.
struct Pickup
{
    int                                             seat = 0;
    platform::ByteStore<kRanges.size(), kCopySize>  copy;              // five Exchange values (byte_scope.h)
    void*                                           copyOf = nullptr;  // the manager the copy belongs to
    bool                                            broken = false;    // a game call raised: the seat's pass is off
    std::unordered_map<std::uint64_t, std::uint8_t> bits;              // carried target bits, by point key
    std::vector<std::uint64_t>                      marked;            // WorldItem keys its pass marked, until granted
    std::unordered_map<std::uint64_t, TwoMarker>    markers;           // its marker widgets, by point key
    std::vector<std::uintptr_t>                     shown;             // sorted: its marker widgets
    std::vector<std::uint64_t>                      markerKeysLogged;
    std::array<std::uint64_t, kFieldActionSlots>    targetsLogged{};
    Counts                                          counts;
};
static_assert(players::kMaxPlayers == 3, "one pickup record per player below");
Pickup g_pickups[players::kMaxPlayers] = {{0}, {1}, {2}};  // the byte stores cannot move: built in place
int g_passSeat = -1;  // the seat whose pass runs now (-1: none); the hooks inside the pass read it

std::vector<Saved>                                  g_saved;    // scratch, reserved: nothing allocates under the game's lock
std::vector<std::pair<std::uint64_t, std::uint8_t>> g_twoNow;   // scratch, reserved
std::unordered_map<std::uint64_t, TwoMarker>        g_twoMarkersNext;  // scratch: the map After builds
std::vector<std::pair<std::uint64_t, std::uintptr_t>> g_twoMarkersNow; // scratch, reserved
std::vector<Doomed>                                 g_doomed;          // for the game's destroy after the walk, reserved
std::vector<std::uintptr_t>                         g_oneMarkers;      // sorted: player 1's point markers after a pass
std::vector<std::uint64_t>                          g_markerKeys;      // scratch: a seat's marker keys, sorted
int                                                 g_hashChecked    = 0;
int                                                 g_hashMismatches = 0;
bool                                                g_hashTrusted    = true;
platform::Every g_status{kStatusEvery};

Pickup& PickupOf(int seat) noexcept
{
    return g_pickups[seat];
}

const std::uint8_t* PointerAt(const void* base, std::size_t offset) noexcept
{
    return static_cast<const std::uint8_t*>(ReadPointer(base, offset));
}

const std::uint8_t* Root() noexcept
{
    return PointerAt(reinterpret_cast<const void*>(g_root), 0);
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"-") : players::unreal::NameText(name);
}

/// Where a manager offset sits in the seat's copy (only offsets inside kRanges), or null without a copy.
std::uint8_t* CopyAt(Pickup& p, std::uint32_t offset) noexcept
{
    for (std::size_t i = 0; i < kRanges.size(); ++i)
    {
        const Range& range = kRanges[i];
        if (offset >= range.offset && offset < range.offset + range.size)
        {
            const auto value = p.copy.Value(i);
            return value.empty() ? nullptr : value.data() + (offset - range.offset);
        }
    }
    return nullptr;
}

template <class T>
void PutInCopy(Pickup& p, std::uint32_t offset, T value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (auto* at = CopyAt(p, offset))
    {
        std::memcpy(at, &value, sizeof(value));
    }
}

template <class T>
T TakeFromCopy(Pickup& p, std::uint32_t offset) noexcept
{
    T value{};
    if (const auto* at = CopyAt(p, offset))
    {
        std::memcpy(&value, at, sizeof(value));
    }
    return value;
}

/// The seat's ranges into the manager, player 1's saved (the copy belongs to `mgr`). False, nothing swapped, if a
/// range could not be read or written. One seat's swap is closed (SwapOut) before another's opens.
bool SwapIn(Pickup& p, void* mgr) noexcept
{
    const auto live = reinterpret_cast<std::uintptr_t>(mgr);
    for (std::size_t i = 0; i < kRanges.size(); ++i)
    {
        if (!p.copy.Retarget(i, live + kRanges[i].offset))
        {
            return false;
        }
    }
    return p.copy.Open();
}

/// Player 1's ranges back; what the game left in them is the seat's copy now. False if a range faulted on the way
/// (player 1's block or the seat's copy may be wrong).
bool SwapOut(Pickup& p) noexcept
{
    p.copy.Close();
    return !p.copy.Faulted();
}

class SharedLock
{
public:
    explicit SharedLock(void* mgr) noexcept
        : lock_(reinterpret_cast<PSRWLOCK>(static_cast<std::uint8_t*>(mgr) + kFieldActionPointLock))
    {
        AcquireSRWLockShared(lock_);
    }
    ~SharedLock() { ReleaseSRWLockShared(lock_); }
    SharedLock(const SharedLock&)            = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    PSRWLOCK lock_;
};

/// The seat's controller first in the world's player controller list for one call, so "the first local player" the
/// game asks for is the seat's; player 1's order put back after. The seat's entry is found by its controller, never by
/// its position. Opens only when the list's first entry resolves to player 1's controller and a later one to the seat's
/// (two Set ranges of platform/byte_scope.h, entry 0 and the seat's entry, each holding the other's), and restores only
/// if the list is still where it was and names them swapped (a list the engine moved or changed in between is left
/// alone). A join appends to the list and a leave closes it up in order, so a seat's place depends on who joined and
/// left before it (player 1 always first); the call's one reader takes the first local player's entry and never looks
/// further (RE-seats-step6-world-rides.md §1). Game thread.
class FirstPlayerScope
{
public:
    explicit FirstPlayerScope(int seat) noexcept
    {
        using Range = platform::ByteContext::Range;
        void*         one     = players::generation::PlayerOneController();
        void*         two     = players::generation::ControllerOf(seat);
        auto*         entries = Entries(two);
        const auto    at      = entries != nullptr ? EntryOf(two, entries) : 0;
        std::uint64_t first = 0, mine = 0;
        if (one == nullptr || entries == nullptr || at <= 0 || scopes::leader_scope::ObjectAt(entries) != one ||
            !platform::CopyGuarded(&first, entries, sizeof(first)) ||
            !platform::CopyGuarded(&mine, entries + 8 * at, sizeof(mine)))
        {
            return;
        }
        const Range swapped[] = {{reinterpret_cast<std::uintptr_t>(entries), sizeof(mine), Kind::Set},
                                 {reinterpret_cast<std::uintptr_t>(entries + 8 * at), sizeof(first), Kind::Set}};
        if (!context_.Define(swapped))
        {
            return;
        }
        std::memcpy(context_.Value(0).data(), &mine, sizeof(mine));
        std::memcpy(context_.Value(1).data(), &first, sizeof(first));
        one_  = one;
        two_  = two;
        at_   = at;
        held_ = context_.Open();
    }
    ~FirstPlayerScope()
    {
        if (!held_)
        {
            return;
        }
        // The list as it is now (the call may have changed it): put back only if it is the same list and still
        // names them swapped; else forgotten (nothing of the old order written anywhere).
        auto* entries = Entries(two_);
        if (reinterpret_cast<std::uintptr_t>(entries) == context_.RangeAt(0).live &&
            scopes::leader_scope::ObjectAt(entries) == two_ && scopes::leader_scope::ObjectAt(entries + 8 * at_) == one_)
        {
            context_.Close();
        }
        else
        {
            context_.Forget();
        }
    }
    FirstPlayerScope(const FirstPlayerScope&)            = delete;
    FirstPlayerScope& operator=(const FirstPlayerScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    /// The most entries searched for a seat's controller (a bound on a list read from the game; it holds the player
    /// controllers, RE-p2-hud.md [ASSUMPTION that nothing else is in it]).
    static constexpr std::int32_t kMaxSearched = 16;

    /// The world's controller list entries (controller -> level -> world), or null when it has fewer than two.
    static std::uint8_t* Entries(const void* controller) noexcept
    {
        const auto* world = PointerAt(PointerAt(controller, kObjectOuter), kObjectOuter);
        if (controller == nullptr || world == nullptr || ReadField<std::int32_t>(world, kWorldControllerCount) < 2)
        {
            return nullptr;
        }
        return const_cast<std::uint8_t*>(PointerAt(world, kWorldControllers));
    }

    /// The index of `controller`'s entry in the list (1.., never 0: that one must be player 1's), or 0 if not found.
    static std::int32_t EntryOf(const void* controller, const std::uint8_t* entries) noexcept
    {
        const auto* world = PointerAt(PointerAt(controller, kObjectOuter), kObjectOuter);
        const auto  count = std::min(ReadField<std::int32_t>(world, kWorldControllerCount), kMaxSearched);
        for (std::int32_t i = 1; i < count; ++i)
        {
            if (scopes::leader_scope::ObjectAt(entries + 8 * i) == controller)
            {
                return i;
            }
        }
        return 0;
    }

    platform::ByteStore<2, 2 * sizeof(std::uint64_t)> context_;
    void*                                             one_  = nullptr;
    void*                                             two_  = nullptr;
    std::int32_t                                      at_   = 0;  // the seat's entry
    bool                                              held_ = false;
};

/// The game's visibility request; inside a seat's pass its screen test sees that seat's view (§12.13).
std::uint64_t RequestVisibility(void* system, void* record) noexcept
{
    const auto original = reinterpret_cast<RequestFn>(g_request.Original());
    if (!players::SeatBuilt(g_passSeat) || !players::generation::OnGameThread())
    {
        return original(system, record);
    }
    Counts& counts = PickupOf(g_passSeat).counts;
    ++counts.requests;
    const FirstPlayerScope first(g_passSeat);
    counts.requestsAsTwo += first.Held() ? 1 : 0;
    return original(system, record);
}

void Before(Pickup& p, void* mgr, const std::uint8_t* items, const std::uint8_t* chests, bool filter) noexcept;
void After(Pickup& p, void* mgr) noexcept;
void DestroyDoomed() noexcept;
void DestroyAllTwo(Pickup& p, Why why) noexcept;

/// Runs a game function on the seat's copy: swapped in with each point's carried bits and marker widget (the resets
/// read the candidates' widgets, §12.15), the shared field-enter flag showing `flagSeen` (the value the game's own call
/// saw), and all put back after. False if it faulted.
bool OnCopy(Pickup& p, void* mgr, ResetFn reset, std::uint8_t flagSeen) noexcept
{
    auto* flag = static_cast<std::uint8_t*>(mgr) + kFieldEnterFlag;
    const std::uint8_t after = *flag;
    if (!SwapIn(p, mgr))
    {
        return false;
    }
    Before(p, mgr, nullptr, nullptr, false);
    *flag         = flagSeen;
    const bool ok = platform::CallGuarded(reset, mgr);
    *flag         = after;
    After(p, mgr);
    const bool out = SwapOut(p);
    DestroyDoomed();
    return ok && out;
}

/// The step of a seat's pass or copy that raised (Break's log).
enum class Step
{
    FieldEnterReset,
    FieldExitClear,
    FieldReset,
    InputStep,
    Update,
    PutBack,
};

void Break(Pickup& p, Step step) noexcept
{
    p.broken = true;
    try
    {
        const wchar_t* const who = players::SeatName(p.seat);
        std::wstring         what;
        switch (step)
        {
        case Step::FieldEnterReset: what = std::format(L"the field-enter reset of {}'s copy", who); break;
        case Step::FieldExitClear:  what = std::format(L"the field-exit clear of {}'s copy", who); break;
        case Step::FieldReset:      what = std::format(L"a field reset of {}'s copy", who); break;
        case Step::InputStep:       what = std::format(L"{}'s prompt input step", who); break;
        case Step::Update:          what = std::format(L"{}'s selection update", who); break;
        case Step::PutBack:         what = L"putting player 1's field-action ranges back"; break;
        }
        const platform::FaultText fault = platform::DescribeFault(platform::LastFault());
        platform::Log(L"field pickup: {} raised an exception ({}); {}'s pass is off from here on", what,
                      static_cast<const wchar_t*>(fault.text), who);
    }
    catch (...)
    {
    }
    DestroyAllTwo(p, kWhyStopped);
    g_oneMarkers.clear();  // the halves show every marker again
}

/// The seat's copy as the game's constructor and field-enter reset leave a block.
void FreshCopy(Pickup& p, void* mgr) noexcept
{
    // The five ranges at this manager, zeroed (SwapIn retargets them: a later manager gets a fresh copy anyway).
    const auto                                            live = reinterpret_cast<std::uintptr_t>(mgr);
    std::array<platform::ByteContext::Range, kRanges.size()> ranges{};
    for (std::size_t i = 0; i < kRanges.size(); ++i)
    {
        ranges[i] = {live + kRanges[i].offset, kRanges[i].size, Kind::Exchange};
    }
    p.copy.Define(ranges);
    PutInCopy<std::int32_t>(p, kCandidateMax, 64);
    PutInCopy<std::int32_t>(p, kTraceMapA, 128);
    PutInCopy<std::int32_t>(p, kTraceMapB, -1);
    for (int i = 0; i < kFieldActionSlots; ++i)
    {
        PutInCopy<std::int32_t>(p, kFieldActionSlotRows + 8 * i, -1);  // FWeakObjectPtr {index -1, serial 0}
    }
    p.copyOf = mgr;
    p.bits.clear();
    p.marked.clear();
    p.targetsLogged.fill(0);
    p.markers.clear();  // none: the last copy's went with it (DropCopy)
    p.shown.clear();
    g_oneMarkers.clear();
    // The reset writes the default slot locations itself (its source vector is not constant).
    if (!OnCopy(p, mgr, g_fieldEnter, 0))
    {
        Break(p, Step::FieldEnterReset);
    }
}

/// Drops the seat's copy; its rows go the game's way (the field-exit clear) when `clear`, and its marker widgets
/// through the game's destroy (only those still in the marker list: after a new manager, the old points are gone).
void DropCopy(Pickup& p, bool clear) noexcept
{
    if (p.copyOf != nullptr && clear && !p.broken &&
        !OnCopy(p, p.copyOf, g_fieldExit, ReadField<std::uint8_t>(p.copyOf, kFieldEnterFlag)))
    {
        Break(p, Step::FieldExitClear);
    }
    DestroyAllTwo(p, kWhyLeft);
    p.copyOf = nullptr;
    p.bits.clear();
    p.marked.clear();
    g_oneMarkers.clear();
}

/// Whether `key` is in the map of `owner` (shaped as `shape`), by the map's own hash (§10.5; platform/tset_find.h). The
/// first lookups are also checked against a walk of the map; a mismatch switches to the walk for good (logged). Both
/// maps share the hash, so they share the trust in it.
bool InMap(const std::uint8_t* owner, const MapShape& shape, std::uint64_t key) noexcept
{
    if (owner == nullptr || key == 0)
    {
        return false;
    }
    const auto               map = reinterpret_cast<std::uintptr_t>(owner + shape.elements);
    const platform::SetShape set{shape.stride, shape.hashNext};
    if (!g_hashTrusted)
    {
        return platform::WalkFind(map, set, key);
    }
    const bool found = platform::HashFind(map, set, key);
    if (g_hashChecked < kHashChecks)
    {
        ++g_hashChecked;
        if (platform::WalkFind(map, set, key) != found)
        {
            ++g_hashMismatches;
            g_hashTrusted = false;
            return !found;
        }
    }
    return found;
}
/// Whether `widget` is a marker in the game's marker list now (an element's weak +0x00 names it).
bool InMarkerList(std::uintptr_t widget) noexcept
{
    const auto list  = reinterpret_cast<std::uintptr_t>(
        PointerAt(reinterpret_cast<const void*>(g_uiSystem), facts::kUIFieldMarkers));
    bool       found = false;
    platform::ForEachSparseElement(list, kMarkerEntry, [widget, &found](std::uintptr_t entry) noexcept {
        found = reinterpret_cast<std::uintptr_t>(scopes::leader_scope::ObjectAt(reinterpret_cast<const void*>(entry))) == widget;
        return !found;
    });
    return found;
}

void Doom(const Pickup& p, std::uintptr_t widget, std::uint64_t weak, std::uint64_t key, Why why) noexcept
{
    try
    {
        g_doomed.push_back({widget, weak, key, why, p.seat});
    }
    catch (...)
    {
    }
}

/// The game's own destroy for each doomed widget still in the marker list (so never one the game destroyed already)
/// and, when its weak value is known, still that object. Each logged. Outside the point lock.
void DestroyDoomed() noexcept
{
    for (const Doomed& doomed : g_doomed)
    {
        const bool live = (doomed.weak == 0 || scopes::leader_scope::StillNames(doomed.weak, reinterpret_cast<const void*>(doomed.widget))) &&
                          InMarkerList(doomed.widget);
        const bool done = live && platform::CallGuarded(g_destroy, reinterpret_cast<void*>(doomed.widget));
        PickupOf(doomed.seat).counts.markersDestroyed += done ? 1 : 0;
        try
        {
            platform::LogDebug(L"field pickup: the marker widget of {} ({}) {}", Name(doomed.key),
                               WhyText(doomed.why, doomed.seat),
                          done   ? L"destroyed by the game's own call"
                          : live ? L"NOT destroyed: the game's call raised an exception"
                                 : L"was gone already");
        }
        catch (...)
        {
        }
    }
    g_doomed.clear();
}

/// The seat's marker widgets as the pass left them in the points, kept by key (with a weak value, made once per
/// widget); made and game-destroyed ones counted. May throw (the map); the caller then drops them all.
void KeepTwoMarkers(Pickup& p)
{
    g_twoMarkersNext.clear();
    for (const auto& [key, widget] : g_twoMarkersNow)
    {
        const auto before = p.markers.find(key);
        const bool same   = before != p.markers.end() && before->second.widget == widget;
        const auto weak   = same ? before->second.weak : scopes::leader_scope::WeakOf(reinterpret_cast<void*>(widget));
        if (weak == 0)
        {
            Doom(p, widget, 0, key, kWhyNoWeak);
            continue;
        }
        const auto [at, added] = g_twoMarkersNext.try_emplace(key, TwoMarker{widget, weak, false});
        if (!added)
        {
            if (at->second.widget != widget)
            {
                Doom(p, widget, weak, key, kWhySecond);
            }
            continue;
        }
        p.counts.markersMade += same ? 0 : 1;
    }
    for (const auto& [key, marker] : p.markers)
    {
        // A widget that left its point inside the pass is the game's to account for: the sweep or a removal destroyed it.
        const auto now = g_twoMarkersNext.find(key);
        p.counts.markersGone += marker.placed && (now == g_twoMarkersNext.end() || now->second.widget != marker.widget) ? 1 : 0;
    }
    p.markers.swap(g_twoMarkersNext);
    p.shown.clear();
    for (const auto& [key, marker] : p.markers)
    {
        p.shown.push_back(marker.widget);
    }
    std::sort(p.shown.begin(), p.shown.end());
}

/// Every one of the seat's marker widgets to the game's destroy (none of the game's code would destroy them).
void DestroyAllTwo(Pickup& p, Why why) noexcept
{
    for (const auto& [key, marker] : p.markers)
    {
        Doom(p, marker.widget, marker.weak, key, why);
    }
    p.markers.clear();
    p.shown.clear();
    DestroyDoomed();
}

/// Before the calls: each point's target bits and marker widget become the seat's (on every live point: the far-marker
/// sweep walks them all, §12.15). With `filter`, every point that is neither a WorldItem nor a chest (§13) gets the disabled
/// bit (only where it is clear). Player 1's are kept for After. The seat's widgets whose point left since are doomed.
void Before(Pickup& p, void* mgr, const std::uint8_t* items, const std::uint8_t* chests, bool filter) noexcept
{
    g_saved.clear();
    for (auto& [key, marker] : p.markers)
    {
        marker.placed = false;
    }
    {
        const SharedLock lock(mgr);
        const auto*      node = PointerAt(mgr, kFieldActionPointList);
        for (int n = 0; node != nullptr && n < kFieldActionMaxPoints; ++n, node = PointerAt(node, kFieldActionNodeNext))
        {
            auto*          point = const_cast<std::uint8_t*>(node) + kFieldActionNodePoint;
            const auto     key   = ReadField<std::uint64_t>(point, kFieldActionPointKey);
            std::uint8_t   flags = point[kFieldActionPointFlags];
            const auto     two   = p.bits.find(key);
            const bool     worldItem = filter && InMap(items, kWorldItemMap, key);
            const bool     chest     = filter && !worldItem && InMap(chests, kChestMap, key);
            const bool     machine   = filter && !worldItem && !chest &&
                                       vending::Admits(ReadField<std::uint64_t>(point, kFieldActionPointPreset));
            const bool     keeper    = filter && !worldItem && !chest && !machine && shop_talk::Admits(key);
            const bool     disable   = filter && !worldItem && !chest && !machine && !keeper && (flags & kDisabledBit) == 0;
            std::uintptr_t oneMarker = 0;
            std::uintptr_t twoMarker = 0;
            if (g_destroy != nullptr)
            {
                std::memcpy(&oneMarker, point + kPointMarker, sizeof(oneMarker));
                if (const auto mine = p.markers.find(key); mine != p.markers.end() && !mine->second.placed)
                {
                    mine->second.placed = true;
                    twoMarker = scopes::leader_scope::StillNames(mine->second.weak, reinterpret_cast<const void*>(mine->second.widget))
                                    ? mine->second.widget
                                    : 0;
                }
                std::memcpy(point + kPointMarker, &twoMarker, sizeof(twoMarker));
            }
            g_saved.push_back({point, key, static_cast<std::uint8_t>(flags & kTargetBits), disable, oneMarker, false});
            p.counts.worldItems += worldItem ? 1 : 0;
            p.counts.chests += chest ? 1 : 0;
            p.counts.vending += machine ? 1 : 0;
            p.counts.shopkeepers += keeper ? 1 : 0;
            p.counts.filtered += disable ? 1 : 0;
            flags = static_cast<std::uint8_t>((flags & ~kTargetBits) | (two != p.bits.end() ? two->second : 0) |
                                              (disable ? kDisabledBit : 0));
            point[kFieldActionPointFlags] = flags;
        }
    }
    for (const auto& [key, marker] : p.markers)
    {
        if (!marker.placed)
        {
            Doom(p, marker.widget, marker.weak, key, kWhyPointLeft);
        }
    }
}

/// After the calls: the seat's target bits and marker widgets are kept by key, player 1's put back, and the disabled
/// bits the pass set cleared. A point registered during the pass gets no target bits and no marker for player 1; player
/// 1's widget of a point removed during the pass is doomed (the removal destroyed the widget in place, the seat's).
void After(Pickup& p, void* mgr) noexcept
{
    std::sort(g_saved.begin(), g_saved.end(), [](const Saved& a, const Saved& b) { return a.point < b.point; });
    g_twoNow.clear();
    g_twoMarkersNow.clear();
    g_oneMarkers.clear();
    {
        const SharedLock lock(mgr);
        const auto*      node = PointerAt(mgr, kFieldActionPointList);
        for (int n = 0; node != nullptr && n < kFieldActionMaxPoints; ++n, node = PointerAt(node, kFieldActionNodeNext))
        {
            auto*      point = const_cast<std::uint8_t*>(node) + kFieldActionNodePoint;
            const auto key   = ReadField<std::uint64_t>(point, kFieldActionPointKey);
            const auto saved = std::lower_bound(g_saved.begin(), g_saved.end(), point,
                                                [](const Saved& s, const std::uint8_t* p) { return s.point < p; });
            const bool known = saved != g_saved.end() && saved->point == point && saved->key == key;
            std::uint8_t flags = point[kFieldActionPointFlags];
            if ((flags & kTargetBits) != 0)
            {
                g_twoNow.emplace_back(key, static_cast<std::uint8_t>(flags & kTargetBits));
            }
            flags = static_cast<std::uint8_t>((flags & ~kTargetBits) | (known ? saved->oneBits : 0));
            if (known && saved->disabled)
            {
                flags = static_cast<std::uint8_t>(flags & ~kDisabledBit);
            }
            point[kFieldActionPointFlags] = flags;
            if (known)
            {
                saved->matched = true;
            }
            if (g_destroy != nullptr)
            {
                std::uintptr_t twoMarker = 0;
                std::memcpy(&twoMarker, point + kPointMarker, sizeof(twoMarker));
                if (twoMarker != 0)
                {
                    g_twoMarkersNow.emplace_back(key, twoMarker);
                }
                const std::uintptr_t oneMarker = known ? saved->oneMarker : 0;
                std::memcpy(point + kPointMarker, &oneMarker, sizeof(oneMarker));
                if (oneMarker != 0)
                {
                    g_oneMarkers.push_back(oneMarker);
                }
            }
        }
    }
    for (const Saved& saved : g_saved)
    {
        if (!saved.matched && saved.oneMarker != 0)
        {
            Doom(p, saved.oneMarker, 0, saved.key, kWhyRemovedInPass);
        }
    }
    std::sort(g_oneMarkers.begin(), g_oneMarkers.end());
    try
    {
        p.bits.clear();
        for (const auto& [key, bits] : g_twoNow)
        {
            p.bits[key] = bits;
        }
    }
    catch (...)
    {
        p.bits.clear();  // the seat's carried targets start over; the game is left as it was
    }
    try
    {
        KeepTwoMarkers(p);
    }
    catch (...)
    {
        // The seat's widgets as the pass left them, all to the game's destroy: none is lost in the list.
        for (const auto& [key, widget] : g_twoMarkersNow)
        {
            Doom(p, widget, 0, key, kWhyStopped);
        }
        p.markers.clear();
        p.shown.clear();
    }
}

void LogMarkers(Pickup& p) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // each change of the seat's markers: Debug
    }
    try
    {
        g_markerKeys.clear();
        for (const auto& [key, marker] : p.markers)
        {
            g_markerKeys.push_back(key);
        }
        std::sort(g_markerKeys.begin(), g_markerKeys.end());
        if (g_markerKeys == p.markerKeysLogged)
        {
            return;
        }
        p.markerKeysLogged = g_markerKeys;
        std::wstring names;
        for (const std::uint64_t key : g_markerKeys)
        {
            names += std::format(L"{}{}", names.empty() ? L"" : L", ", Name(key));
        }
        platform::LogDebug(L"field pickup: {}'s own marker widgets: {}", players::SeatName(p.seat),
                           names.empty() ? L"none" : names);
    }
    catch (...)
    {
    }
}

/// The input step's poll gate, mirrored in the game's order (§12.12 item 2). `slot0Only` as the game picks the poll.
/// False when the poll is skipped; `faulted` when a game call raised.
bool PollGate(const std::uint8_t* root, bool& slot0Only, bool& faulted) noexcept
{
    const auto* gs = PointerAt(root, kRootGameState);
    if (gs == nullptr || PointerAt(root, kRootModeHolder) == nullptr)
    {
        return false;
    }
    std::uint8_t flag = 0;
    if (!platform::CallGuarded(g_testFlag, flag, kGameFlagBlocksPoll))
    {
        faulted = true;
        return false;
    }
    const bool open    = flag == 0 && ReadField<std::int32_t>(gs, kGsBlockA) == 0 &&
                         ReadField<std::int32_t>(gs, kGsBlockB) == 0;
    const auto allow   = ReadField<std::uint32_t>(gs, kGsAllowA) + ReadField<std::uint32_t>(gs, kGsAllowB);
    const auto slot0   = ReadField<std::uint32_t>(gs, kGsSlot0A) + ReadField<std::uint32_t>(gs, kGsSlot0B);
    slot0Only          = slot0 != 0;
    if (!open && allow == 0 && slot0 == 0)
    {
        return false;
    }
    std::uint8_t mode = 0;
    if (!platform::CallGuarded(g_modeClass, mode))
    {
        faulted = true;
        return false;
    }
    const auto* entries = PointerAt(reinterpret_cast<const void*>(g_uiSystem), facts::kUICommandController);
    if (mode == kModeBlocksPoll || entries == nullptr || ReadField<std::int32_t>(entries, kCmdOpenPending) >= 0)
    {
        return false;
    }
    std::uint8_t positive = 0;
    if (!platform::CallGuarded(g_uiEntry, positive))
    {
        faulted = true;
        return false;
    }
    if (positive != 0)
    {
        return false;
    }
    // The pause button, in pad 2's view (the button scope is open). The game's own copy: the held test releases
    // the reference the copy adds.
    alignas(8) std::array<std::byte, 24> key{};
    std::uint8_t held = 0;
    if (!platform::CallGuarded(g_copyKey, static_cast<void*>(key.data()), reinterpret_cast<const void*>(g_pauseKey)) ||
        !platform::CallGuarded(g_isHeld, held, static_cast<void*>(key.data())))
    {
        faulted = true;
        return false;
    }
    return held == 0;
}

/// The input step's FieldAction part for the seat (§12.1): the gate, the poll into the mod's own buffers, the
/// latch and the press tick, the last two always. False if a game call raised.
bool InputStep(Pickup& p, void* mgr, const std::uint8_t* root, float dt) noexcept
{
    if (ReadField<std::uint8_t>(root, kRootStage) < 8)
    {
        return true;  // the game runs no FieldAction input either
    }
    std::uint8_t held[kFieldActionSlots]{};
    std::uint8_t pressed[kFieldActionSlots]{};
    bool         slot0Only = false;
    bool         faulted   = false;
    if (PollGate(root, slot0Only, faulted))
    {
        ++p.counts.polled;
        if (!platform::CallGuarded(slot0Only ? g_pollSlot0 : g_poll8, mgr, held, pressed))
        {
            return false;
        }
    }
    if (faulted)
    {
        return false;
    }
    auto*              slotHeld = const_cast<std::uint8_t*>(PointerAt(root, kRootSlotHeldOwner));
    const std::uint8_t saved    = ReadField<std::uint8_t>(slotHeld, kSlotHeldByte);
    const bool         ok       = platform::CallGuarded(g_latch, mgr, held, pressed) && platform::CallGuarded(g_tickSlots, mgr, dt);
    if (slotHeld != nullptr)
    {
        platform::WriteGuarded(slotHeld + kSlotHeldByte, &saved, sizeof(saved));
    }
    return ok;
}

void LogTargets(Pickup& p) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // each prompt change: Debug
    }
    std::array<std::uint64_t, kFieldActionSlots> now{};
    for (int i = 0; i < kFieldActionSlots; ++i)
    {
        now[i] = TakeFromCopy<std::uint64_t>(p, kFieldActionSlotTargets + 8 * i);
    }
    if (now == p.targetsLogged)
    {
        return;
    }
    p.targetsLogged = now;
    try
    {
        std::wstring slots;
        for (int i = 0; i < kFieldActionSlots; ++i)
        {
            if (now[i] != 0)
            {
                slots += std::format(L"{}slot {} {}", slots.empty() ? L"" : L", ", i, Name(now[i]));
            }
        }
        platform::LogDebug(L"field pickup: {}'s prompts: {}", players::SeatName(p.seat), slots.empty() ? L"none" : slots);
    }
    catch (...)
    {
    }
}

/// The status line, once a minute while a seat counted something: one line per seat that did.
void LogStatus() noexcept
{
    const auto now = Clock::now();
    bool       any = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        any = any || !(PickupOf(seat).counts == Counts{});
    }
    if (!any || !g_status.Due(now))
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Pickup&       p   = PickupOf(seat);
        const Counts& c   = p.counts;
        const auto    who = players::SeatName(seat);
        if (c == Counts{})
        {
            continue;
        }
        try
        {
            platform::LogDebug(L"field pickup (60 s): {} pass(es) (poll open in {}), {} pickup(s) by {}, {} field reset(s) "
                          L"of {}'s copy; {} visibility request(s), {} with {} as the first local player; per "
                          L"pass on average {} WorldItem point(s), {} chest point(s), {} vending machine(s) and {} shopkeeper(s) left open, {} other point(s) disabled for the pass; "
                          L"skipped: scopes {}, member {}; WorldItem and chest lookups by {} ({} checked, {} mismatch(es)); {}'s "
                          L"marker widgets: {} now, {} made and {} destroyed by the game's own code, {} by the mod's call of "
                          L"its destroy; player 1's point markers now {}",
                          c.passes, c.polled, c.executed, who, c.fieldResets, who, c.requests, c.requestsAsTwo, who,
                          c.passes != 0 ? c.worldItems / c.passes : 0,
                          c.passes != 0 ? c.chests / c.passes : 0,
                          c.passes != 0 ? c.vending / c.passes : 0,
                          c.passes != 0 ? c.shopkeepers / c.passes : 0,
                          c.passes != 0 ? c.filtered / c.passes : 0, c.skips[kSkipScopes],
                          c.skips[kSkipMember], g_hashTrusted ? L"the map's hash" : L"a walk of the map", g_hashChecked,
                          g_hashMismatches, who, p.markers.size(), c.markersMade, c.markersGone,
                          c.markersDestroyed, g_oneMarkers.size());
        }
        catch (...)
        {
        }
        p.counts = {};
    }
}

/// The seat's pass itself, with its scopes open (the seat's player in, the copy belongs to `mgr`).
void Pass(Pickup& p, void* mgr, float dt) noexcept
{
    seat_ladder::Args args;
    args.seat = p.seat;
    if (!seat_ladder::FillMember(args, players::generation::MemberOf(p.seat)))
    {
        ++p.counts.skips[kSkipMember];
        return;
    }
    const seat_ladder::SeatScopes scopes(seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons |
                                             seat_ladder::Leader | seat_ladder::LeaderType,
                                         args);
    if (!scopes.Held())
    {
        ++p.counts.skips[kSkipScopes];
        return;
    }
    const auto* root  = Root();
    const auto* items  = PointerAt(root, kRootWorldItems);
    const auto* chests = PointerAt(root, kRootChests);
    if (!SwapIn(p, mgr))
    {
        ++p.counts.skips[kSkipScopes];
        return;
    }
    ++p.counts.passes;
    g_passSeat = p.seat;
    Before(p, mgr, items, chests, true);
    const bool input  = InputStep(p, mgr, root, dt);
    const bool update = input && platform::CallGuarded(g_update, mgr, dt);
    After(p, mgr);
    const bool out = SwapOut(p);
    g_passSeat     = -1;
    DestroyDoomed();
    if (!input || !update || !out)
    {
        Break(p, !input ? Step::InputStep : !update ? Step::Update : Step::PutBack);
        return;
    }
    LogTargets(p);
    LogMarkers(p);
}

/// The game's field enter/exit reset ran on the live block: the same on each seat's copy of that block, in seat
/// order, then its latches zeroed (the resets leave them, §12.12 item 3) and its carried bits dropped (the points may
/// be recycled).
void AfterFieldReset(void* mgr, ResetFn reset, std::uint8_t flagSeen) noexcept
{
    if (mgr == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Pickup& p = PickupOf(seat);
        if (mgr != p.copyOf || p.broken)
        {
            continue;
        }
        if (!OnCopy(p, mgr, reset, flagSeen))
        {
            Break(p, Step::FieldReset);
            continue;
        }
        for (std::uint32_t i = 0; i < 3 * kFieldActionSlots; ++i)
        {
            PutInCopy<std::uint8_t>(p, kFieldActionSlotHeld + i, 0);
        }
        PutInCopy<std::uint8_t>(p, kFieldActionPending, 0);
        PutInCopy<std::uint8_t>(p, kReExecuted, 0);
        p.bits.clear();
        p.marked.clear();
        if (reset == g_fieldExit)
        {
            // The field's points are removed by their owners, whose removal destroys only the widget in place (player 1's).
            DestroyAllTwo(p, kWhyFieldExit);
        }
        ++p.counts.fieldResets;
    }
}

void FieldEnter(void* mgr) noexcept
{
    const std::uint8_t seen = ReadField<std::uint8_t>(mgr, kFieldEnterFlag);
    reinterpret_cast<ResetFn>(g_enterCall.Original())(mgr);
    AfterFieldReset(mgr, g_fieldEnter, seen);
}

// --- The pass's hooks --------------------------------------------------------------------------------------

/// The game mode tick's call of the selection update: the game's (player 1's), then each seat's pass.
void UpdateCall(void* mgr, float dt) noexcept
{
    reinterpret_cast<TickFn>(g_updateCall.Original())(mgr, dt);
    if (mgr == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    AfterUpdate(mgr, dt);
    if (g_observer.afterUpdate != nullptr)
    {
        g_observer.afterUpdate(mgr);
    }
}

/// WorldItemMgr's mark by key: a key a seat's pass marks is noted for its grant, as that seat's.
std::uint8_t MarkByKey(void* items, std::uint64_t key) noexcept
{
    const auto caller    = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto taken     = reinterpret_cast<MarkByKeyFn>(g_markByKey.Original())(items, key);
    const int  seat      = taken != 0 && players::generation::OnGameThread() ? PassSeat() : -1;
    const bool playerTwo = seat >= 1;
    if (playerTwo)
    {
        NoteMarked(seat, key);
    }
    if (g_observer.marked != nullptr)
    {
        g_observer.marked(key, taken, caller, playerTwo);
    }
    return taken;
}

/// The WorldItem tick's call of the grant: an item a seat picked is granted with that seat's controller as the primary
/// one, its only player read (the item-get rumble, §12.12 item 5).
void Acquire(void* items, void* entry) noexcept
{
    const auto key =
        ReadField<std::uint64_t>(static_cast<const std::uint8_t*>(entry) - kFieldActionEntryKeyBack, 0);
    const int  seat      = players::generation::OnGameThread() ? TakeMarked(key) : -1;
    const bool playerTwo = seat >= 1;
    if (g_observer.granting != nullptr)
    {
        g_observer.granting(key, playerTwo);
    }
    std::optional<scopes::primary_controller::Scope> primary;
    if (playerTwo)
    {
        primary.emplace(seat);
    }
    reinterpret_cast<AcquireFn>(g_acquireCall.Original())(items, entry);
    if (g_observer.granted != nullptr)
    {
        g_observer.granted(key, playerTwo);  // inside the scope: the observer may read who the rumble went to
    }
}

void FieldExit(void* mgr) noexcept
{
    const std::uint8_t seen = ReadField<std::uint8_t>(mgr, kFieldEnterFlag);
    reinterpret_cast<ResetFn>(g_exitCall.Original())(mgr);
    AfterFieldReset(mgr, g_fieldExit, seen);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto root    = image.Rebase(kEndGameRoot, sizeof(void*));
    const auto ui      = image.Rebase(facts::kEndUISystem, facts::kUICommandController + sizeof(void*));
    const auto pause   = image.Rebase(kPauseMenuKey, 24);
    const auto find    = [&image](const platform::FunctionFact& fact) {
        const auto at = platform::FindFunction(image, fact);
        return at ? *at : std::uintptr_t{0};
    };
    g_testFlag   = reinterpret_cast<TestFlagFn>(find(kTestFlag));
    g_modeClass  = reinterpret_cast<ModeClassFn>(find(kModeClass));
    g_uiEntry    = reinterpret_cast<UIEntryFn>(find(kUIEntry));
    g_isHeld     = reinterpret_cast<IsHeldFn>(find(kIsHeld));
    g_copyKey    = reinterpret_cast<CopyKeyFn>(find(kInputKeyCopy));
    g_pollSlot0  = reinterpret_cast<PollFn>(find(kPollSlot0));
    g_poll8      = reinterpret_cast<PollFn>(find(kPoll8));
    g_latch      = reinterpret_cast<PollFn>(find(kLatch));
    g_tickSlots  = reinterpret_cast<TickFn>(find(kTickSlots));
    g_update     = reinterpret_cast<TickFn>(find(kUpdate));
    g_fieldEnter = reinterpret_cast<ResetFn>(find(kFieldEnter));
    g_fieldExit  = reinterpret_cast<ResetFn>(find(kFieldExit));
    g_destroy    = reinterpret_cast<DestroyFn>(find(kDestroy));  // optional: without it the markers stay shared
    const bool functions = g_testFlag && g_modeClass && g_uiEntry && g_isHeld && g_copyKey && g_pollSlot0 && g_poll8 &&
                           g_latch && g_tickSlots && g_update && g_fieldEnter && g_fieldExit;
    bool resets = false;
    if (root && ui && pause && functions)
    {
        g_root     = *root;
        g_uiSystem = *ui;
        g_pauseKey = *pause;
        g_saved.reserve(kFieldActionMaxPoints);
        g_twoNow.reserve(kFieldActionMaxPoints);
        g_twoMarkersNow.reserve(kFieldActionMaxPoints);
        g_oneMarkers.reserve(kFieldActionMaxPoints);
        g_doomed.reserve(3 * kFieldActionMaxPoints);  // Before's, After's and KeepTwoMarkers': one per point each
        resets = g_enterCall.InstallResearch(image, kFieldEnterCall, kFieldEnter.address,
                                             reinterpret_cast<std::uintptr_t>(&FieldEnter)) &&
                 g_exitCall.InstallResearch(image, kFieldExitCall, kFieldExit.address,
                                            reinterpret_cast<std::uintptr_t>(&FieldExit));
        if (!resets)
        {
            g_enterCall.Restore();  // both or neither: player 2's rows must not outlive a field change
        }
    }
    // The three hooks: the pass runs only through the update's; the mark and the grant carry its edges.
    bool hooks = false;
    if (resets)
    {
        const auto markAt    = platform::FindFunction(image, kMarkByKey);
        const auto acquireAt = image.Rebase(kAcquireCall, 5);
        const auto acquire   = image.Rebase(kAcquire);
        hooks = g_updateCall.InstallResearch(image, kUpdateCall, kUpdate.address,
                                             reinterpret_cast<std::uintptr_t>(&UpdateCall)) && markAt &&
                g_markByKey.Install(*markAt,
                                    image.EntryInstruction(kMarkByKey.address, kMarkByKey.entryBytes.first(kMarkFirst)),
                                    reinterpret_cast<std::uintptr_t>(&MarkByKey)) &&
                acquireAt && acquire && g_acquireCall.Install(*acquireAt, *acquire, reinterpret_cast<std::uintptr_t>(&Acquire));
    }
    g_ready = resets && hooks;
    bool screen = false;
    if (g_ready)
    {
        const auto at = platform::FindFunction(image, kRequest);
        screen        = at &&
                 g_request.Install(*at,
                                   image.EntryInstruction(kRequest.address, kRequest.entryBytes.first(kRequestFirst)),
                                   reinterpret_cast<std::uintptr_t>(&RequestVisibility));
    }
    platform::Log(L"field pickup: {}; the screen test {}; marker widgets {}",
                  g_ready ? L"ready (player 2 picks up collectibles and materials, opens chests, uses vending machines and shopkeepers "
                            L"through the game's own field prompts, a second pass of its selection and press)"
                  : resets ? L"UNAVAILABLE (a hook is not what the research read: the update's call, the mark's entry or the grant's call)"
                           : L"UNAVAILABLE (a check failed: the root, the UI system, a function or a field reset's call site)",
                  screen ? L"sees player 2's view in player 2's pass"
                         : L"is NOT redirected (the request's entry is not what the research read): player 2's offers follow "
                           L"player 1's screen",
                  g_destroy != nullptr ? L"per player (each point's widget swapped too; player 2's destroyed by the game's own call)"
                                       : L"SHARED (the game's destroy is not what the research read): both players write one "
                                         L"widget per point");
}

void SetHookObserver(const HookObserver& observer) noexcept
{
    g_observer = observer;
}

void AfterUpdate(void* mgr, float dt) noexcept
{
    if (!g_ready || mgr == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    // The seats in order: each one's copy kept, made or dropped, and its pass run to the end before the next one's.
    bool passed = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Pickup&              p   = PickupOf(seat);
        const wchar_t* const who = players::SeatName(seat);
        const bool in = players::generation::Active(seat) && players::generation::MemberOf(seat) != nullptr;
        if (p.copyOf != nullptr && (!in || p.copyOf != mgr))
        {
            DropCopy(p, p.copyOf == mgr);  // a new manager: the old copy is dropped without a call
            if (in)
            {
                platform::Log(L"field pickup: {}'s copy dropped (a new manager)", who);
            }
            else
            {
                platform::Log(L"field pickup: {}'s copy cleared ({} left)", who, who);
            }
        }
        if (!in || p.broken)
        {
            continue;
        }
        if (p.copyOf == nullptr)
        {
            FreshCopy(p, mgr);
            if (p.broken)
            {
                continue;
            }
            platform::Log(L"field pickup: {}'s copy of the prompt slots made", who);
        }
        Pass(p, mgr, dt);
        passed = true;
    }
    if (passed)
    {
        LogStatus();
    }
}

void RowWeaks(int seat, Weaks& out) noexcept
{
    out.fill(0);
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    Pickup& p = PickupOf(seat);
    if (p.copyOf == nullptr || g_passSeat >= 0)
    {
        return;
    }
    for (int i = 0; i < kFieldActionSlots; ++i)
    {
        const auto weak = TakeFromCopy<std::uint64_t>(p, kFieldActionSlotRows + 8 * i);
        out[i]          = static_cast<std::int32_t>(weak) >= 0 ? weak : 0;
    }
}

bool InPass() noexcept
{
    return g_passSeat >= 0;
}

int PassSeat() noexcept
{
    return g_passSeat;
}

bool IsChestEntry(std::uint64_t popKey) noexcept
{
    return InMap(PointerAt(Root(), kRootChests), kChestByPopKeyMap, popKey);
}

void NoteMarked(int seat, std::uint64_t key) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    Pickup& p = PickupOf(seat);
    try
    {
        if (std::find(p.marked.begin(), p.marked.end(), key) == p.marked.end())
        {
            p.marked.push_back(key);
            ++p.counts.executed;
        }
    }
    catch (...)
    {
    }
}

int MarkerOwner(const void* widget) noexcept
{
    const auto at = reinterpret_cast<std::uintptr_t>(widget);
    if (at == 0)
    {
        return -1;
    }
    bool copies = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Pickup& p = PickupOf(seat);
        copies          = copies || p.copyOf != nullptr;
        if (p.copyOf != nullptr && std::binary_search(p.shown.begin(), p.shown.end(), at))
        {
            return seat;
        }
    }
    return copies && std::binary_search(g_oneMarkers.begin(), g_oneMarkers.end(), at) ? 0 : -1;
}

int TakeMarked(std::uint64_t key) noexcept
{
    // The lowest seat that marked the key takes the grant. Two seats marking one key in one frame (the game grants it
    // once) leave the other seat's entry here until a field reset or that seat leaves clears it: harmless, the key is
    // taken.
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Pickup&    p  = PickupOf(seat);
        const auto it = std::find(p.marked.begin(), p.marked.end(), key);
        if (it != p.marked.end())
        {
            p.marked.erase(it);
            return seat;
        }
    }
    return -1;
}

void Remove() noexcept
{
    g_ready       = false;  // the pass runs no more
    bool restored = true;
    for (platform::EntryRedirect* entry : {&g_request, &g_markByKey})
    {
        restored = entry->Restore() && restored;
    }
    for (platform::CallRedirect* call : {&g_acquireCall, &g_updateCall, &g_exitCall, &g_enterCall})
    {
        restored = call->Restore() && restored;
    }
    if (!restored)
    {
        platform::Log(L"field pickup: a call or entry was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::world::field_pickup
