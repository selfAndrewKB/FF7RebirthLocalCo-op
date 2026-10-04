#include "player_two/field_pickup.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::field_pickup {

namespace {

using Clock = std::chrono::steady_clock;

// The objects (RE-p2-field-interact.md §1.1, §12; this build).
constexpr std::uintptr_t kGameRoot          = 0x1490FEDE8;  // pointer global: the engine root
constexpr std::uint32_t  kRootStage         = 0x70;         // u8: the input step's FieldAction part runs from 8
constexpr std::uint32_t  kRootGameState     = 0x170;        // GS
constexpr std::uint32_t  kRootModeHolder    = 0x1B8;        // read by the mode-class gate (no null check of its own)
constexpr std::uint32_t  kRootActionManager = 0x1A0;        // FieldActionMgr
constexpr std::uint32_t  kRootSlotHeldOwner = 0x210;        // -> +8: a u8 TickSlots writes (§12.1)
constexpr std::uint32_t  kSlotHeldByte      = 0x08;
constexpr std::uint32_t  kRootWorldItems    = 0x288;        // WorldItemMgr
constexpr std::uintptr_t kEndUISystem       = 0x1493496E0;  // the EndUISystem object itself
constexpr std::uint32_t  kUIEntries         = 0xB8;         // -> +0x5D0: i32, the poll needs it < 0
constexpr std::uint32_t  kUIEntryValue      = 0x5D0;
constexpr std::uintptr_t kPauseMenuKey      = 0x1491E44C0;  // InputKey24 "PauseMenu" (the poll needs it not held)

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
constexpr std::uint32_t kSlotRows        = 0x483A8;  // 8 x FWeakObjectPtr {index, serial}
constexpr std::uint32_t kSlotTargets     = 0x48608;  // 8 x FName
constexpr std::uint32_t kSlotLatches     = 0x48730;  // held, pressed, executed: 3 x 8 bytes
constexpr std::uint32_t kPendingFlag     = 0x487D0;  // u8
constexpr std::uint32_t kReExecuted      = 0x487E8;  // u8
constexpr std::uint32_t kFieldEnterFlag  = 0x487E9;  // u8, shared: the field-enter reset's own flag
constexpr std::uint32_t kPointList       = 0x48320;  // the live list's head node; node+0x08 next, point = node+0x18
constexpr std::uint32_t kNodeNext        = 0x08;
constexpr std::uint32_t kNodePoint       = 0x18;
constexpr std::uint32_t kPointLock       = 0x487F0;  // SRWLOCK
constexpr int           kSlots           = 8;
constexpr int           kMaxPoints       = 1024;     // the pool's size

// A point.
constexpr std::uint32_t kPointKey       = 0x0C;   // FName
constexpr std::uint32_t kPointFlags     = 0x104;  // u8
constexpr std::uint8_t  kTargetBits     = 0x06;   // bit 1 slot target, bit 2 runner-up: carried per player (§12.3)
constexpr std::uint8_t  kDisabledBit    = 0x10;   // the game's "point disabled": the candidate query skips it

// WorldItemMgr's map of items (§10.5): a stock TMap, 0xD0-byte elements, the key FName at +0.
constexpr std::uint32_t kItemElements   = 0x90;
constexpr std::uint32_t kItemCount      = 0x98;  // i32 ArrayNum
constexpr std::uint32_t kItemBitsInline = 0xA0;
constexpr std::uint32_t kItemBitsHeap   = 0xB0;
constexpr std::uint32_t kItemFreeCount  = 0xC4;  // i32 NumFreeIndices
constexpr std::uint32_t kItemHashInline = 0xC8;
constexpr std::uint32_t kItemHashHeap   = 0xD0;
constexpr std::uint32_t kItemHashSize   = 0xD8;  // i32
constexpr std::uint32_t kItemStride     = 0xD0;
constexpr std::uint32_t kItemHashNext   = 0xC0;  // i32
constexpr std::int32_t  kMaxItems       = 16384;
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
constexpr std::uint8_t kCopyKeyBytes[] = {0x48, 0x8B, 0x02, 0x48, 0x89, 0x01, 0x48, 0x8B,
                                          0x42, 0x08, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8B};
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
constexpr platform::FunctionFact kCopyKey{L"InputKey24_CopyAddRef", 0x14078C2F4, kCopyKeyBytes};
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
// local player (the first controller in World->ControllerList whose player is local), always player 1's. Inside
// player 2's pass the request runs with player 2's controller first in that list. Entry: mov [rsp+18h],rbx (the
// 5 bytes replaced), then push rbp/rsi/rdi/r12/r13/r14/r15.
constexpr std::uintptr_t kRequest        = 0x140ADE688;
constexpr std::uint8_t   kRequestBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::size_t    kRequestFirst   = 5;
// UObjectBase's outer, and UWorld's ControllerList: TArray<TWeakObjectPtr<AController>> (8-byte entries).
constexpr std::uint32_t  kObjectOuter      = 0x20;
constexpr std::uint32_t  kWorldControllers = 0x1F0;
constexpr std::uint32_t  kWorldControllerCount = 0x1F8;

using TestFlagFn  = std::uint8_t (*)(std::uint8_t bit);
using ModeClassFn = std::uint8_t (*)();
using UIEntryFn   = std::uint8_t (*)();
using IsHeldFn    = std::uint8_t (*)(void* key);  // releases the key's reference
using CopyKeyFn   = void (*)(void* to, const void* from);  // 24 bytes, one more reference
using PollFn      = void (*)(void* mgr, std::uint8_t* held, std::uint8_t* pressed);
using TickFn      = void (*)(void* mgr, float dt);
using ResetFn     = void (*)(void* mgr);
using RequestFn   = std::uint64_t (*)(void* system, void* record);  // the handle in eax

constexpr auto kStatusEvery = std::chrono::seconds(60);

/// Why a pass did not run (counted for the status line).
enum Skip : int
{
    kSkipScopes,  // a scope did not hold
    kSkipMember,  // player 2's member or its type could not be read
    kSkipCount
};

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
platform::CallRedirect g_enterCall;
platform::CallRedirect g_exitCall;
platform::EntryRedirect g_request;
bool           g_ready       = false;

// Game thread.
std::array<std::byte, kCopySize>               g_copy{};      // player 2's ranges (player 1's while swapped in)
void*                                          g_copyOf  = nullptr;  // the manager the copy belongs to
bool                                           g_inPass  = false;
bool                                           g_broken  = false;
std::unordered_map<std::uint64_t, std::uint8_t> g_twoBits;    // player 2's carried target bits, by point key
struct Saved
{
    std::uint8_t* point;
    std::uint64_t key;       // the point's key then: a pool node reused during the pass is another point
    std::uint8_t  oneBits;   // player 1's target bits
    bool          disabled;  // the pass set the disabled bit
};
std::vector<Saved>                                  g_saved;    // reserved: nothing allocates under the game's lock
std::vector<std::pair<std::uint64_t, std::uint8_t>> g_twoNow;   // scratch, reserved
std::vector<std::uint64_t>                          g_marked;   // WorldItem keys player 2's pass marked, until granted
std::array<std::uint64_t, kSlots>                   g_targetsLogged{};
int                                                 g_hashChecked    = 0;
int                                                 g_hashMismatches = 0;
bool                                                g_hashTrusted    = true;

// The status line's counts since the last line.
struct Counts
{
    std::uint32_t passes = 0, polled = 0, filtered = 0, worldItems = 0, executed = 0, fieldResets = 0;
    std::uint32_t requests = 0, requestsAsTwo = 0;  // visibility requests in the pass; those with player 2 first
    std::array<std::uint32_t, kSkipCount> skips{};
    bool operator==(const Counts&) const = default;
};
Counts            g_counts;
Clock::time_point g_statusAt{};

template <class T>
T Read(const void* base, std::size_t offset) noexcept
{
    T value{};
    if (base != nullptr)
    {
        platform::CopyGuarded(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    }
    return value;
}

const std::uint8_t* PointerAt(const void* base, std::size_t offset) noexcept
{
    return reinterpret_cast<const std::uint8_t*>(Read<std::uintptr_t>(base, offset));
}

const std::uint8_t* Root() noexcept
{
    return PointerAt(reinterpret_cast<const void*>(g_root), 0);
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"-") : players::unreal::NameText(name);
}

/// Where a manager offset sits in the copy (only offsets inside kRanges).
constexpr std::size_t CopyIndex(std::uint32_t offset) noexcept
{
    std::size_t at = 0;
    for (const Range& range : kRanges)
    {
        if (offset >= range.offset && offset < range.offset + range.size)
        {
            return at + (offset - range.offset);
        }
        at += range.size;
    }
    return kCopySize;
}

template <class T>
void PutInCopy(std::uint32_t offset, T value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(g_copy.data() + CopyIndex(offset), &value, sizeof(value));
}

template <class T>
T TakeFromCopy(std::uint32_t offset) noexcept
{
    T value{};
    std::memcpy(&value, g_copy.data() + CopyIndex(offset), sizeof(value));
    return value;
}

/// Exchanges the manager's per-player ranges with the copy (the same call puts them back).
void Exchange(void* mgr) noexcept
{
    auto*       live = static_cast<std::byte*>(mgr);
    std::size_t at   = 0;
    for (const Range& range : kRanges)
    {
        std::swap_ranges(live + range.offset, live + range.offset + range.size, g_copy.data() + at);
        at += range.size;
    }
}

class SharedLock
{
public:
    explicit SharedLock(void* mgr) noexcept : lock_(reinterpret_cast<PSRWLOCK>(static_cast<std::uint8_t*>(mgr) + kPointLock))
    {
        AcquireSRWLockShared(lock_);
    }
    ~SharedLock() { ReleaseSRWLockShared(lock_); }
    SharedLock(const SharedLock&)            = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    PSRWLOCK lock_;
};

/// Player 2's controller first in the world's controller list for one call, so "the first local player" the game asks
/// for is player 2's; player 1's order put back after. Opens only when the list's first two entries resolve to player
/// 1's and player 2's controllers, and restores only if they still name them swapped (a list the engine changed in
/// between is left alone). Game thread.
class FirstPlayerScope
{
public:
    FirstPlayerScope() noexcept
    {
        void* one = generation::PlayerOneController();
        void* two = generation::PlayerTwoController();
        auto* entries = Entries(two);
        if (one == nullptr || entries == nullptr || leader_scope::ObjectAt(entries) != one ||
            leader_scope::ObjectAt(entries + 8) != two || !Swap(entries))
        {
            return;
        }
        one_  = one;
        two_  = two;
        held_ = true;
    }
    ~FirstPlayerScope()
    {
        if (!held_)
        {
            return;
        }
        // The list as it is now (the call may have changed it): put back only if it still names them swapped.
        auto* entries = Entries(two_);
        if (entries != nullptr && leader_scope::ObjectAt(entries) == two_ && leader_scope::ObjectAt(entries + 8) == one_)
        {
            Swap(entries);
        }
    }
    FirstPlayerScope(const FirstPlayerScope&)            = delete;
    FirstPlayerScope& operator=(const FirstPlayerScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    /// The world's controller list entries (controller -> level -> world), or null when it has fewer than two.
    static std::uint8_t* Entries(const void* controller) noexcept
    {
        const auto* world = PointerAt(PointerAt(controller, kObjectOuter), kObjectOuter);
        if (controller == nullptr || world == nullptr || Read<std::int32_t>(world, kWorldControllerCount) < 2)
        {
            return nullptr;
        }
        return const_cast<std::uint8_t*>(PointerAt(world, kWorldControllers));
    }

    /// Exchanges entries [0] and [1] (guarded writes; on a fault the first is written back).
    static bool Swap(std::uint8_t* entries) noexcept
    {
        std::uint64_t first = 0, second = 0;
        if (!platform::CopyGuarded(&first, entries, sizeof(first)) ||
            !platform::CopyGuarded(&second, entries + 8, sizeof(second)) ||
            !platform::WriteGuarded(entries, &second, sizeof(second)))
        {
            return false;
        }
        if (!platform::WriteGuarded(entries + 8, &first, sizeof(first)))
        {
            platform::WriteGuarded(entries, &first, sizeof(first));
            return false;
        }
        return true;
    }

    void* one_  = nullptr;
    void* two_  = nullptr;
    bool  held_ = false;
};

/// The game's visibility request; inside player 2's pass its screen test sees player 2's view (§12.13).
std::uint64_t RequestVisibility(void* system, void* record) noexcept
{
    const auto original = reinterpret_cast<RequestFn>(g_request.Original());
    if (!g_inPass || !generation::OnGameThread())
    {
        return original(system, record);
    }
    ++g_counts.requests;
    const FirstPlayerScope first;
    g_counts.requestsAsTwo += first.Held() ? 1 : 0;
    return original(system, record);
}

/// Runs a game function on player 2's copy: swapped in, the shared field-enter flag showing `flagSeen` (the value
/// the game's own call saw) and put back after. False if it faulted.
bool OnCopy(void* mgr, ResetFn reset, std::uint8_t flagSeen) noexcept
{
    auto* flag = static_cast<std::uint8_t*>(mgr) + kFieldEnterFlag;
    const std::uint8_t after = *flag;
    Exchange(mgr);
    *flag         = flagSeen;
    const bool ok = platform::CallGuarded(reset, mgr);
    *flag         = after;
    Exchange(mgr);
    return ok;
}

void Break(const wchar_t* what) noexcept
{
    g_broken = true;
    try
    {
        const platform::FaultText fault = platform::DescribeFault(platform::LastFault());
        platform::Log(L"field pickup: {} raised an exception ({}); player 2's pass is off from here on", what,
                      static_cast<const wchar_t*>(fault.text));
    }
    catch (...)
    {
    }
}

/// Player 2's copy as the game's constructor and field-enter reset leave a block.
void FreshCopy(void* mgr) noexcept
{
    g_copy.fill(std::byte{0});
    PutInCopy<std::int32_t>(kCandidateMax, 64);
    PutInCopy<std::int32_t>(kTraceMapA, 128);
    PutInCopy<std::int32_t>(kTraceMapB, -1);
    for (int i = 0; i < kSlots; ++i)
    {
        PutInCopy<std::int32_t>(kSlotRows + 8 * i, -1);  // FWeakObjectPtr {index -1, serial 0}
    }
    g_copyOf = mgr;
    g_twoBits.clear();
    g_marked.clear();
    g_targetsLogged.fill(0);
    // The reset writes the default slot locations itself (its source vector is not constant).
    if (!OnCopy(mgr, g_fieldEnter, 0))
    {
        Break(L"the field-enter reset of player 2's copy");
    }
}

/// Drops player 2's copy; its rows go the game's way (the field-exit clear) when `clear`.
void DropCopy(bool clear) noexcept
{
    if (g_copyOf != nullptr && clear && !g_broken && !OnCopy(g_copyOf, g_fieldExit, Read<std::uint8_t>(g_copyOf, kFieldEnterFlag)))
    {
        Break(L"the field-exit clear of player 2's copy");
    }
    g_copyOf = nullptr;
    g_twoBits.clear();
    g_marked.clear();
}

/// Whether `key` is in WorldItemMgr's map, by the map's own hash (§10.5). The first lookups are also checked
/// against a walk of the map; a mismatch switches to the walk for good (logged).
bool HashFind(const std::uint8_t* items, std::uint64_t key) noexcept
{
    if (Read<std::int32_t>(items, kItemCount) == Read<std::int32_t>(items, kItemFreeCount))
    {
        return false;
    }
    const auto* elements = PointerAt(items, kItemElements);
    const auto  size     = Read<std::int32_t>(items, kItemHashSize);
    const auto* heap     = PointerAt(items, kItemHashHeap);
    const auto* buckets  = heap != nullptr ? heap : items + kItemHashInline;
    if (elements == nullptr || size <= 0 || (size & (size - 1)) != 0)
    {
        return false;
    }
    const auto index = static_cast<std::uint32_t>(key);
    const auto lo    = index & 0xFFFF;
    const auto hi    = index >> 16;
    const auto hash  = static_cast<std::uint32_t>(key >> 32) + hi + lo + (lo >> 4) + ((lo + 8 * hi) << 16);
    auto       i     = Read<std::int32_t>(buckets, 4 * (hash & static_cast<std::uint32_t>(size - 1)));
    for (int steps = 0; i >= 0 && i < kMaxItems && steps < kMaxItems; ++steps)
    {
        const auto* element = elements + static_cast<std::size_t>(i) * kItemStride;
        if (Read<std::uint64_t>(element, 0) == key)
        {
            return true;
        }
        i = Read<std::int32_t>(element, kItemHashNext);
    }
    return false;
}

bool WalkFind(const std::uint8_t* items, std::uint64_t key) noexcept
{
    const auto* elements = PointerAt(items, kItemElements);
    const auto  count    = std::clamp(Read<std::int32_t>(items, kItemCount), 0, kMaxItems);
    const auto* heapBits = PointerAt(items, kItemBitsHeap);
    const auto* bits     = heapBits != nullptr ? heapBits : items + kItemBitsInline;
    for (std::int32_t i = 0; elements != nullptr && i < count; ++i)
    {
        if ((Read<std::uint32_t>(bits, static_cast<std::size_t>(i / 32) * 4) >> (i % 32) & 1) != 0 &&
            Read<std::uint64_t>(elements, static_cast<std::size_t>(i) * kItemStride) == key)
        {
            return true;
        }
    }
    return false;
}

bool IsWorldItem(const std::uint8_t* items, std::uint64_t key) noexcept
{
    if (items == nullptr || key == 0)
    {
        return false;
    }
    if (!g_hashTrusted)
    {
        return WalkFind(items, key);
    }
    const bool found = HashFind(items, key);
    if (g_hashChecked < kHashChecks)
    {
        ++g_hashChecked;
        if (WalkFind(items, key) != found)
        {
            ++g_hashMismatches;
            g_hashTrusted = false;
            return !found;
        }
    }
    return found;
}

/// Before the calls: each point's target bits become player 2's, and every point that is not a WorldItem gets the
/// disabled bit (only where it is clear). Player 1's bits are kept for After.
void Before(void* mgr, const std::uint8_t* items) noexcept
{
    g_saved.clear();
    const SharedLock lock(mgr);
    const auto*      node = PointerAt(mgr, kPointList);
    for (int n = 0; node != nullptr && n < kMaxPoints; ++n, node = PointerAt(node, kNodeNext))
    {
        auto*          point = const_cast<std::uint8_t*>(node) + kNodePoint;
        const auto     key   = Read<std::uint64_t>(point, kPointKey);
        std::uint8_t   flags = point[kPointFlags];
        const auto     two   = g_twoBits.find(key);
        const bool     worldItem = IsWorldItem(items, key);
        const bool     disable   = !worldItem && (flags & kDisabledBit) == 0;
        g_saved.push_back({point, key, static_cast<std::uint8_t>(flags & kTargetBits), disable});
        g_counts.worldItems += worldItem ? 1 : 0;
        g_counts.filtered += disable ? 1 : 0;
        flags = static_cast<std::uint8_t>((flags & ~kTargetBits) | (two != g_twoBits.end() ? two->second : 0) |
                                          (disable ? kDisabledBit : 0));
        point[kPointFlags] = flags;
    }
}

/// After the calls: player 2's target bits are kept by key, player 1's put back, and the disabled bits the pass set
/// cleared. A point registered during the pass gets no target bits for player 1.
void After(void* mgr) noexcept
{
    std::sort(g_saved.begin(), g_saved.end(), [](const Saved& a, const Saved& b) { return a.point < b.point; });
    g_twoNow.clear();
    {
        const SharedLock lock(mgr);
        const auto*      node = PointerAt(mgr, kPointList);
        for (int n = 0; node != nullptr && n < kMaxPoints; ++n, node = PointerAt(node, kNodeNext))
        {
            auto*      point = const_cast<std::uint8_t*>(node) + kNodePoint;
            const auto key   = Read<std::uint64_t>(point, kPointKey);
            const auto saved = std::lower_bound(g_saved.begin(), g_saved.end(), point,
                                                [](const Saved& s, const std::uint8_t* p) { return s.point < p; });
            const bool known = saved != g_saved.end() && saved->point == point && saved->key == key;
            std::uint8_t flags = point[kPointFlags];
            if ((flags & kTargetBits) != 0)
            {
                g_twoNow.emplace_back(key, static_cast<std::uint8_t>(flags & kTargetBits));
            }
            flags = static_cast<std::uint8_t>((flags & ~kTargetBits) | (known ? saved->oneBits : 0));
            if (known && saved->disabled)
            {
                flags = static_cast<std::uint8_t>(flags & ~kDisabledBit);
            }
            point[kPointFlags] = flags;
        }
    }
    try
    {
        g_twoBits.clear();
        for (const auto& [key, bits] : g_twoNow)
        {
            g_twoBits[key] = bits;
        }
    }
    catch (...)
    {
        g_twoBits.clear();  // player 2's carried targets start over; the game is left as it was
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
    const bool open    = flag == 0 && Read<std::int32_t>(gs, kGsBlockA) == 0 && Read<std::int32_t>(gs, kGsBlockB) == 0;
    const auto allow   = Read<std::uint32_t>(gs, kGsAllowA) + Read<std::uint32_t>(gs, kGsAllowB);
    const auto slot0   = Read<std::uint32_t>(gs, kGsSlot0A) + Read<std::uint32_t>(gs, kGsSlot0B);
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
    const auto* entries = PointerAt(reinterpret_cast<const void*>(g_uiSystem), kUIEntries);
    if (mode == kModeBlocksPoll || entries == nullptr || Read<std::int32_t>(entries, kUIEntryValue) >= 0)
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

/// The input step's FieldAction part for player 2 (§12.1): the gate, the poll into the mod's own buffers, the
/// latch and the press tick, the last two always. False if a game call raised.
bool InputStep(void* mgr, const std::uint8_t* root, float dt) noexcept
{
    if (Read<std::uint8_t>(root, kRootStage) < 8)
    {
        return true;  // the game runs no FieldAction input either
    }
    std::uint8_t held[kSlots]{};
    std::uint8_t pressed[kSlots]{};
    bool         slot0Only = false;
    bool         faulted   = false;
    if (PollGate(root, slot0Only, faulted))
    {
        ++g_counts.polled;
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
    const std::uint8_t saved    = Read<std::uint8_t>(slotHeld, kSlotHeldByte);
    const bool         ok       = platform::CallGuarded(g_latch, mgr, held, pressed) && platform::CallGuarded(g_tickSlots, mgr, dt);
    if (slotHeld != nullptr)
    {
        platform::WriteGuarded(slotHeld + kSlotHeldByte, &saved, sizeof(saved));
    }
    return ok;
}

void LogTargets() noexcept
{
    std::array<std::uint64_t, kSlots> now{};
    for (int i = 0; i < kSlots; ++i)
    {
        now[i] = TakeFromCopy<std::uint64_t>(kSlotTargets + 8 * i);
    }
    if (now == g_targetsLogged)
    {
        return;
    }
    g_targetsLogged = now;
    try
    {
        std::wstring slots;
        for (int i = 0; i < kSlots; ++i)
        {
            if (now[i] != 0)
            {
                slots += std::format(L"{}slot {} {}", slots.empty() ? L"" : L", ", i, Name(now[i]));
            }
        }
        platform::Log(L"field pickup: player 2's prompts: {}", slots.empty() ? L"none" : slots);
    }
    catch (...)
    {
    }
}

void LogStatus() noexcept
{
    const auto now = Clock::now();
    if (now - g_statusAt < kStatusEvery || g_counts == Counts{})
    {
        return;
    }
    g_statusAt = now;
    try
    {
        platform::Log(L"field pickup (60 s): {} pass(es) (poll open in {}), {} pickup(s) by player 2, {} field reset(s) "
                      L"of player 2's copy; {} visibility request(s), {} with player 2 as the first local player; per "
                      L"pass on average {} WorldItem point(s) left open, {} other point(s) disabled for the pass; "
                      L"skipped: scopes {}, member {}; WorldItem lookup by {} ({} checked, {} mismatch(es))",
                      g_counts.passes, g_counts.polled, g_counts.executed, g_counts.fieldResets, g_counts.requests,
                      g_counts.requestsAsTwo,
                      g_counts.passes != 0 ? g_counts.worldItems / g_counts.passes : 0,
                      g_counts.passes != 0 ? g_counts.filtered / g_counts.passes : 0, g_counts.skips[kSkipScopes],
                      g_counts.skips[kSkipMember], g_hashTrusted ? L"the map's hash" : L"a walk of the map", g_hashChecked,
                      g_hashMismatches);
    }
    catch (...)
    {
    }
    g_counts = {};
}

/// The pass itself, with the scopes open (player 2 in, the copy belongs to `mgr`).
void Pass(void* mgr, float dt) noexcept
{
    void*      member     = generation::Member();
    const auto memberType = party_member::PlayerTypeOf(member);
    const auto memberWeak = leader_scope::WeakOf(member);
    if (!memberType || memberWeak == 0)
    {
        ++g_counts.skips[kSkipMember];
        return;
    }
    const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(generation::PlayerTwoController()));
    const primary_controller::Scope primary;
    if (!sticks.Held() || !primary.Held())
    {
        ++g_counts.skips[kSkipScopes];
        return;
    }
    const pad_buttons::ButtonScope      buttons;
    const leader_scope::LeaderScope     leader(memberWeak);
    const leader_scope::LeaderTypeScope type(*memberType);
    if (!buttons.Held() || !leader.Held() || !type.Held())
    {
        ++g_counts.skips[kSkipScopes];
        return;
    }
    const auto* root  = Root();
    const auto* items = PointerAt(root, kRootWorldItems);
    ++g_counts.passes;
    g_inPass = true;
    Exchange(mgr);
    Before(mgr, items);
    const bool input  = InputStep(mgr, root, dt);
    const bool update = input && platform::CallGuarded(g_update, mgr, dt);
    After(mgr);
    Exchange(mgr);
    g_inPass = false;
    if (!input || !update)
    {
        Break(input ? L"player 2's selection update" : L"player 2's prompt input step");
        return;
    }
    LogTargets();
}

/// The game's field enter/exit reset ran on the live block: the same on player 2's copy, then its latches zeroed
/// (the resets leave them, §12.12 item 3) and its carried bits dropped (the points may be recycled).
void AfterFieldReset(void* mgr, ResetFn reset, std::uint8_t flagSeen) noexcept
{
    if (mgr == nullptr || mgr != g_copyOf || g_broken || !generation::OnGameThread())
    {
        return;
    }
    if (!OnCopy(mgr, reset, flagSeen))
    {
        Break(L"a field reset of player 2's copy");
        return;
    }
    for (std::uint32_t i = 0; i < 3 * kSlots; ++i)
    {
        PutInCopy<std::uint8_t>(kSlotLatches + i, 0);
    }
    PutInCopy<std::uint8_t>(kPendingFlag, 0);
    PutInCopy<std::uint8_t>(kReExecuted, 0);
    g_twoBits.clear();
    g_marked.clear();
    ++g_counts.fieldResets;
}

void FieldEnter(void* mgr) noexcept
{
    const std::uint8_t seen = Read<std::uint8_t>(mgr, kFieldEnterFlag);
    reinterpret_cast<ResetFn>(g_enterCall.Original())(mgr);
    AfterFieldReset(mgr, g_fieldEnter, seen);
}

void FieldExit(void* mgr) noexcept
{
    const std::uint8_t seen = Read<std::uint8_t>(mgr, kFieldEnterFlag);
    reinterpret_cast<ResetFn>(g_exitCall.Original())(mgr);
    AfterFieldReset(mgr, g_fieldExit, seen);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto root    = image.Rebase(kGameRoot, sizeof(void*));
    const auto ui      = image.Rebase(kEndUISystem, kUIEntries + sizeof(void*));
    const auto pause   = image.Rebase(kPauseMenuKey, 24);
    const auto find    = [&image](const platform::FunctionFact& fact) {
        const auto at = platform::FindFunction(image, fact);
        return at ? *at : std::uintptr_t{0};
    };
    g_testFlag   = reinterpret_cast<TestFlagFn>(find(kTestFlag));
    g_modeClass  = reinterpret_cast<ModeClassFn>(find(kModeClass));
    g_uiEntry    = reinterpret_cast<UIEntryFn>(find(kUIEntry));
    g_isHeld     = reinterpret_cast<IsHeldFn>(find(kIsHeld));
    g_copyKey    = reinterpret_cast<CopyKeyFn>(find(kCopyKey));
    g_pollSlot0  = reinterpret_cast<PollFn>(find(kPollSlot0));
    g_poll8      = reinterpret_cast<PollFn>(find(kPoll8));
    g_latch      = reinterpret_cast<PollFn>(find(kLatch));
    g_tickSlots  = reinterpret_cast<TickFn>(find(kTickSlots));
    g_update     = reinterpret_cast<TickFn>(find(kUpdate));
    g_fieldEnter = reinterpret_cast<ResetFn>(find(kFieldEnter));
    g_fieldExit  = reinterpret_cast<ResetFn>(find(kFieldExit));
    const bool functions = g_testFlag && g_modeClass && g_uiEntry && g_isHeld && g_copyKey && g_pollSlot0 && g_poll8 &&
                           g_latch && g_tickSlots && g_update && g_fieldEnter && g_fieldExit;
    const auto call = [&image](platform::CallRedirect& redirect, std::uintptr_t site, const platform::FunctionFact& target,
                               std::uintptr_t replacement) {
        const auto at = image.Rebase(site, 5);
        const auto to = image.Rebase(target.address);
        return at && to && redirect.Install(*at, *to, replacement);
    };
    bool resets = false;
    if (root && ui && pause && functions)
    {
        g_root     = *root;
        g_uiSystem = *ui;
        g_pauseKey = *pause;
        g_saved.reserve(kMaxPoints);
        g_twoNow.reserve(kMaxPoints);
        resets = call(g_enterCall, kFieldEnterCall, kFieldEnter, reinterpret_cast<std::uintptr_t>(&FieldEnter)) &&
                 call(g_exitCall, kFieldExitCall, kFieldExit, reinterpret_cast<std::uintptr_t>(&FieldExit));
        if (!resets)
        {
            g_enterCall.Restore();  // both or neither: player 2's rows must not outlive a field change
        }
    }
    g_ready = resets;
    bool screen = false;
    if (g_ready)
    {
        const auto at = image.Rebase(kRequest);
        screen        = at && image.BytesMatch(kRequest, kRequestBytes) &&
                 g_request.Install(*at, std::span<const std::uint8_t>(kRequestBytes).first(kRequestFirst),
                                   reinterpret_cast<std::uintptr_t>(&RequestVisibility));
    }
    platform::Log(L"field pickup: {}; the screen test {}",
                  g_ready ? L"ready (player 2 picks up collectibles and materials through the game's own field prompts, a "
                            L"second pass of its selection and press)"
                          : L"UNAVAILABLE (a check failed: the root, the UI system, a function or a field reset's call site)",
                  screen ? L"sees player 2's view in player 2's pass"
                         : L"is NOT redirected (the request's entry is not what the research read): player 2's offers follow "
                           L"player 1's screen");
}

void AfterUpdate(void* mgr, float dt) noexcept
{
    if (!g_ready || mgr == nullptr || !generation::OnGameThread())
    {
        return;
    }
    const bool in = generation::Active() && generation::Member() != nullptr;
    if (g_copyOf != nullptr && (!in || g_copyOf != mgr))
    {
        DropCopy(g_copyOf == mgr);  // a new manager: the old copy is dropped without a call
        platform::Log(L"field pickup: player 2's copy {}", in ? L"dropped (a new manager)" : L"cleared (player 2 left)");
    }
    if (!in || g_broken)
    {
        return;
    }
    if (g_copyOf == nullptr)
    {
        FreshCopy(mgr);
        if (g_broken)
        {
            return;
        }
        platform::Log(L"field pickup: player 2's copy of the prompt slots made");
    }
    Pass(mgr, dt);
    LogStatus();
}

void RowWeaks(Weaks& out) noexcept
{
    out.fill(0);
    if (g_copyOf == nullptr || g_inPass)
    {
        return;
    }
    for (int i = 0; i < kSlots; ++i)
    {
        const auto weak = TakeFromCopy<std::uint64_t>(kSlotRows + 8 * i);
        out[i]          = static_cast<std::int32_t>(weak) >= 0 ? weak : 0;
    }
}

bool InPass() noexcept
{
    return g_inPass;
}

void NoteMarked(std::uint64_t key) noexcept
{
    try
    {
        if (std::find(g_marked.begin(), g_marked.end(), key) == g_marked.end())
        {
            g_marked.push_back(key);
            ++g_counts.executed;
        }
    }
    catch (...)
    {
    }
}

bool TakeMarked(std::uint64_t key) noexcept
{
    const auto it = std::find(g_marked.begin(), g_marked.end(), key);
    if (it == g_marked.end())
    {
        return false;
    }
    g_marked.erase(it);
    return true;
}

}  // namespace lss::player_two::field_pickup
