#include "player_two/trigger_two.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "player_two/traversal_watch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::player_two::trigger_two {

namespace {

// 1.004 addresses (1.005 through replaced-p2-trigger.json and replaced-p2-trigger-pass.json). The field tick's per-tracker
// body was read in both exes (RE-p2-trigger-overlap.md §5.1): the same frame layout, buffer offsets and order.

// FieldTick's `call TriggerTracker_DiffAndDispatchEnterLeave(ctx, tracker, &overlaps, &keys, &map)` (E8 inside one block
// in both builds; 1.005 0x140935A31). `keys` and `map` are the field tick's scratch structures, reset before each tracker.
constexpr std::uintptr_t kDispatchCall    = 0x140A562B1;
constexpr std::uint8_t   kDispatchBytes[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xE2, 0x06, 0x00, 0x00, 0x48, 0x8B, 0xC4,
                                             0x48, 0x89, 0x58, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08};
constexpr platform::FunctionFact kDispatch{L"TriggerTracker_DiffAndDispatchEnterLeave", 0x140A42FAC, kDispatchBytes};
using DispatchFn = std::uintptr_t (*)(void* context, void* tracker, void* overlaps, void* keys, void* map);

// The diff's calls of TriggerMgr_OnTrackerEnter_Filter / _Leave_Filter(manager, FName key, owner, weak actor): `owner`
// is the tracker's owner, the trigger actor. 1.004 enter +0x6 of its block, leave +0xD (crosses); 1.005 enter 0x1409369BD
// +0xD (crosses), leave 0x14093690B +0xB.
constexpr std::uintptr_t kEnterFilterCall = 0x140A43416;
constexpr std::uintptr_t kLeaveFilterCall = 0x140A4352D;
constexpr std::uint8_t   kEnterFilterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                0x74, 0x24, 0x18, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xB6, 0x07};
constexpr std::uint8_t   kLeaveFilterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                0x74, 0x24, 0x18, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xF2, 0x09};
constexpr platform::FunctionFact kEnterFilter{L"TriggerMgr_OnTrackerEnter_Filter", 0x1409DE760, kEnterFilterBytes};
constexpr platform::FunctionFact kLeaveFilter{L"TriggerMgr_OnTrackerLeave_Filter", 0x1409DE524, kLeaveFilterBytes};
// The 4th argument differs (RE-p2-traversal-L1.md §1.1): the enter's is an FOverlapResult* (weak actor at +0) or null,
// the leave's a plain AActor* (0 if the tracker's weak had gone stale).
using FilterFn = std::uintptr_t (*)(void* manager, std::uint64_t key, void* owner, void* actorArg);

// Traversal L1 (RE-p2-traversal-L1.md §8). The routers find a field-action actor by the key, the overlapped actor's
// Tags[0], in Square Enix's tag registry (1.005 0x140F48A40); the mod looks it up the same way.
constexpr std::uint8_t kFindActorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xF0, 0x88, 0x2D, 0x01,
                                            0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0xB9, 0xD5, 0xAB, 0xFF, 0x48};
constexpr platform::FunctionFact kFindActor{L"EndActorTagRegistry_FindActorByTag", 0x140DD6634, kFindActorBytes};
using FindActorFn = void* (*)(const std::uint64_t* key);

// The classes admitted for player 2 (research/RE-p2-traversal-classes.md §7): native vtables, which placed Blueprint
// actors keep. Each registers on the leader's controller list through the shared trigger enter (checked live at the
// slot below); its register/unregister reads only the leader, so the same delivery serves all of them. 1.005 vtables
// through replaced-traversal-classes.json.
struct AdmittedClass
{
    const wchar_t* name;
    std::uintptr_t vtable;
    std::uint32_t  storedWeak = 0;   // a character weak the class stores at register and unregisters from (shadowed too)
    std::uintptr_t unregister = 0;   // that class's unregister override, checked at its slot
    bool           enterEffectOnly = false;  // zip lines: only the auto-start kind (bUseEnterEffect +0x770)
    bool           logsUseFields   = false;  // one-shot moves: the register's +0x848 / +0x86C, logged per delivery
};
constexpr AdmittedClass kAdmitted[] = {
    {L"ladder", 0x145940748},
    {L"climbing wall", 0x145832BE8},
    {L"crawl", 0x1457D8960},
    {L"squeeze", 0x1466E8B68},
    {L"beam", 0x145872C88},
    {L"monkey bars", 0x1466EF050},
    {L"lifting rope", 0x1466FE5D0},
    {L"spider move", 0x1467095F8},
    {L"zip line", 0x1458BA188, 0, 0, true},
    {L"pole slide", 0x1458B41F8, 0x758, 0x143642998},
    {L"one-shot move", 0x14580EF00, 0x840, 0x141812E50, false, true},
    {L"wall run", 0x146707D88},
};
constexpr std::size_t   kClassCount     = std::size(kAdmitted);
constexpr std::uint32_t kZiplineEnterEffect = 0x770;  // u8 bUseEnterEffect: set = starts on approach, else a button prompt
constexpr std::uint32_t kOneShotUseA     = 0x848;  // OneShotMove fields its register resets [UNKNOWN meaning]: logged
constexpr std::uint32_t kOneShotUseB     = 0x86C;
constexpr std::uintptr_t kTriggerEnter   = 0x1409DC534;  // End2FieldActionActor_OnTriggerEnter (1.005 0x140D813D0)
inline constexpr platform::PerBuild<std::uint32_t> kEnterSlot{0x710, 0x718};
inline constexpr platform::PerBuild<std::uint32_t> kUnregisterSlot{0x728, 0x730};
constexpr std::uint64_t kNoWeak = 0x00000000FFFFFFFFull;  // FWeakObjectPtr reset: index -1, serial 0

// The wall-run move state's camera pick (RE §3.13): LeaderActionComp_FindWallRunActorEntered_Flag2A1(lac) returns the
// first actor on the LeaderActionComp's wall-run list (+0x14F0) whose REAL entered flag is set. Its only call
// (1.004 0x1434372CA, 1.005 0x14315631A, inside one 16-byte block in both) is taken over: for player 2's
// LeaderActionComp (*(PC2+0x598)) player 2's flags are swapped in around it.
constexpr std::uintptr_t kWallRunPickCall = 0x1434372CA;
constexpr std::uint8_t   kWallRunPickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kWallRunPick{L"LeaderActionComp_FindWallRunActorEntered_Flag2A1", 0x143CFFE9C,
                                              kWallRunPickBytes};
using WallRunPickFn = void* (*)(void* leaderActionComp);
constexpr std::uint32_t kControllerLeaderAction = 0x598;   // PC -> its LeaderActionComp
constexpr std::uint32_t kWallRunList            = 0x14F0;  // TArray<FWeakObjectPtr>: data, then the count at +0x8
constexpr std::int32_t  kWallRunListMax         = 32;

// End2FieldActionActorBase's entered flag: its trigger enter needs 0 and sets 1, its leave needs 1 and sets 0. One per
// actor, so player 2's is kept aside (the shadow) and swapped in around player 2's deliveries; the field is player 1's.
constexpr std::uint32_t kEnteredFlag = 0x2A1;

// The game's allocator (stock FMemory), for the overlap array handed to the diff: the diff may empty it (TArray::Empty,
// which frees through GMalloc), so it must be the game's memory. FieldTick frees its own copy with the same Free.
constexpr std::uint8_t kMallocBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                         0x00, 0x00, 0x00, 0xE8, 0x48, 0xFB, 0x86, 0x01};  // as command_menu_two
constexpr std::uint8_t kFreeBytes[]   = {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA0, 0x03, 0x00, 0x00, 0x57, 0xB8, 0x50,
                                         0x00, 0x00, 0x00, 0xE8, 0x4C, 0xF6, 0x90, 0x01, 0x48, 0x2B, 0xE0, 0x4C};
constexpr platform::FunctionFact kMalloc{L"FMemory_Malloc", 0x14083F3D8, kMallocBytes};
constexpr platform::FunctionFact kFree{L"FMemory_Free", 0x14079F8D0, kFreeBytes};
using MallocFn = void* (*)(std::size_t size, std::uint32_t alignment);
using FreeFn   = void (*)(void* block);

// The scratch set's element clear, which FieldTick calls before each tracker when the set holds anything (1.004
// 0x140A5632C, 1.005 0x140935B44).
constexpr std::uint8_t kSetClearBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                           0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0xB8};
constexpr platform::FunctionFact kSetClear{L"FieldTick's scratch set clear", 0x1409B7DCC, kSetClearBytes};
using SetClearFn = void (*)(void* set);

// Data.
constexpr std::uintptr_t kRegistryWeak = 0x1470C94C0;  // FWeakObjectPtr to the trigger registry (FieldTick's `this`)
constexpr std::uintptr_t kOverlapLock  = 0x1491DE5D8;  // CRITICAL_SECTION held while FieldTick reads an overlap datum
constexpr std::uintptr_t kSweepFlagA   = 0x1493BE6DA;  // either set: FieldTick reads sweep results instead
constexpr std::uintptr_t kSweepFlagB   = 0x1493BE6D9;

// The registry's world getter (AActor/UObject virtual; 1.005 moved it 8 bytes, RE-p2-trigger-overlap.md §6).
inline constexpr platform::PerBuild<std::uint32_t> kGetWorldSlot{0x160, 0x168};
using GetWorldFn = void* (*)(void* object);

// Stock UE 4.26 UWorld async trace data (the same in both builds): the frame counter and two double-buffered
// FAsyncTraceData sets of 0x88 bytes, each a chunked array of 64 datums per chunk.
constexpr std::uint32_t kWorldFrame        = 0x3A8;
constexpr std::uint32_t kTraceChunks       = 0x298;  // FTraceDatum chunks
constexpr std::uint32_t kTraceChunkCount   = 0x2A0;
constexpr std::uint32_t kOverlapChunks     = 0x2A8;  // FOverlapDatum chunks
constexpr std::uint32_t kOverlapChunkCount = 0x2B0;
constexpr std::uint32_t kBufferStride      = 0x88;
constexpr std::int32_t  kPerChunk          = 64;
constexpr std::uint32_t kOverlapDatumSize  = 0x150;
constexpr std::uint32_t kOverlapResults    = 0x140;  // TArray<FOverlapResult>, 24 bytes each
constexpr std::uint32_t kTraceDatumSize    = 0x170;
constexpr std::uint32_t kTraceHits         = 0x150;  // TArray<FHitResult>, 0x90 bytes each
constexpr std::uint32_t kHitSize           = 0x90;
constexpr std::uint32_t kHitActor          = 0x6C;   // FHitResult::Actor (weak)
constexpr std::uint32_t kResultSize        = 24;     // FOverlapResult: weak actor, weak component, item, blocking

// The trigger's tracker (RE-p2-trigger-overlap.md §2.3).
constexpr std::uint32_t kTriggerTracker = 0x230;
constexpr std::uint32_t kTrackerEnabled = 0x375;
constexpr std::uint32_t kTrackerHandle  = 0x378;  // FTraceHandle {u32 frame, i32 index}

// FieldTick's scratch structures (§5.1): the keys (TArray<FName>, inline 64) and the set.
constexpr std::uint32_t kKeysNum      = 0x208;
constexpr std::uint32_t kKeysMax      = 0x20C;
constexpr std::uint32_t kSetNum       = 0x608;
constexpr std::uint32_t kSetMax       = 0x60C;
constexpr std::uint32_t kSetHashCount = 0x620;
constexpr std::uint32_t kSetFirstFree = 0x628;
constexpr std::uint32_t kSetNumFree   = 0x62C;

struct ResultArray  // TArray<FOverlapResult> with the game's allocator
{
    std::byte*   data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

enum class Read : std::uint8_t
{
    Overlap,
    Sweep,
    NoResult,  // the handle is not last frame's (the game skips the diff then)
};

platform::CallRedirect g_dispatchCall;
platform::CallRedirect g_enterCall;
platform::CallRedirect g_leaveCall;
std::uintptr_t         g_enterSite = 0, g_leaveSite = 0, g_enterTarget = 0, g_leaveTarget = 0;
bool                   g_filtersPending = false;
bool                   g_filtersPlaced  = false;
bool                   g_ready          = false;  // every function and datum found
std::optional<platform::GameImage> g_image;

MallocFn             g_malloc   = nullptr;
FreeFn               g_free     = nullptr;
SetClearFn           g_setClear = nullptr;
void*                g_weakGet  = nullptr;
const void*          g_registryWeak = nullptr;
CRITICAL_SECTION*    g_overlapLock  = nullptr;
const std::uint8_t*  g_sweepFlagA   = nullptr;
const std::uint8_t*  g_sweepFlagB   = nullptr;
FindActorFn          g_findActor    = nullptr;  // null: no class is admitted, every event of player 2's is held back
std::uintptr_t       g_classVt[kClassCount] = {};  // live vtables of the admitted classes that passed their checks (0: not)
platform::CallRedirect g_wallRunCall;
const void*            g_wallRunPicked = nullptr;  // player 2's last camera pick, logged on change

// Game thread only.
struct Counts
{
    unsigned passes = 0, overlapReads = 0, sweepReads = 0, noResult = 0, skipped = 0, enters = 0, leaves = 0;
    unsigned deliveredEnters = 0, deliveredLeaves = 0, refused = 0;
};

// Player 2's per-actor state, by the actor's weak value: the entered flag, and for classes that store a character the
// stored weak. Kept only while the flag is set. It belongs to one trigger's tracker (its last-pass list says which
// actors it is inside), so it is dropped when player 2's trigger is another object.
struct Shadow
{
    std::uint8_t  flag   = 0;
    std::uint64_t stored = kNoWeak;
};
std::unordered_map<std::uint64_t, Shadow> g_shadow;
std::uint64_t                                   g_shadowOwner = 0;
bool                                            g_delivering  = false;  // inside the game's filter for player 2
constexpr std::size_t                           kShadowPrune  = 32;  // past this, entries of destroyed actors go

/// Holds g_delivering for one delivery; reset on every way out.
struct Delivering
{
    Delivering() noexcept { g_delivering = true; }
    ~Delivering() { g_delivering = false; }
    Delivering(const Delivering&)            = delete;
    Delivering& operator=(const Delivering&) = delete;
};
Counts                                g_counts{};
std::int32_t                          g_lastCount = -1;
bool                                  g_skipLogged = false;
bool                                  g_wallRunCapLogged = false;
std::chrono::steady_clock::time_point g_summaryAt{};

using WeakGetFn = void* (*)(const void* weak);

void* Resolve(const void* weak) noexcept
{
    return g_weakGet != nullptr ? reinterpret_cast<WeakGetFn>(g_weakGet)(weak) : nullptr;
}

void* TriggerOf(const void* controller) noexcept
{
    return ReadPointer(controller, facts::kControllerTrigger);
}

void* TrackerOf(const void* controller) noexcept
{
    auto* trigger = static_cast<std::byte*>(TriggerOf(controller));
    return trigger != nullptr ? trigger + kTriggerTracker : nullptr;
}

/// Room for `count` zeroed 24-byte results in `out`, in the game's memory (none for 0). False if the allocation failed.
bool Allocate(ResultArray& out, std::int32_t count) noexcept
{
    if (count <= 0)
    {
        return true;
    }
    auto* block = static_cast<std::byte*>(g_malloc(static_cast<std::size_t>(count) * kResultSize, 0));
    if (block == nullptr)
    {
        return false;
    }
    std::memset(block, 0, static_cast<std::size_t>(count) * kResultSize);
    out = {block, 0, count};
    return true;
}

/// The datum chunk entry for `index` in `world`'s buffer for last frame, as UWorld's async trace code addresses it.
const std::byte* Datum(const std::byte* world, std::uint32_t chunks, std::uint32_t chunkCount, std::uint32_t size,
                       std::int32_t index) noexcept
{
    const auto frame  = ReadField<std::int32_t>(world, kWorldFrame);
    const auto buffer = static_cast<std::uint32_t>(kBufferStride * ((frame + 1) % 2));
    const std::int32_t chunk = index / kPerChunk;
    const std::int32_t slot  = index % kPerChunk;
    if (chunk < 0 || chunk >= ReadField<std::int32_t>(world, buffer + chunkCount) || static_cast<std::uint32_t>(slot) > 63)
    {
        return nullptr;
    }
    const auto* table = static_cast<const std::byte*>(ReadPointer(world, buffer + chunks));
    const auto* base  = table != nullptr ? static_cast<const std::byte*>(ReadPointer(table, chunk * 8)) : nullptr;
    return base != nullptr ? base + static_cast<std::size_t>(size) * slot : nullptr;
}

/// Player 2's tracker's query result, read as FieldTick reads it (RE-p2-trigger-overlap.md §5.1), into `out`.
Read ReadResult(void* tracker, ResultArray& out) noexcept
{
    void* registry = Resolve(g_registryWeak);
    if (registry == nullptr)
    {
        return Read::NoResult;
    }
    const auto getWorld = reinterpret_cast<GetWorldFn>(ReadPointer(ReadPointer(registry, 0), kGetWorldSlot));
    const auto* world   = getWorld != nullptr ? static_cast<const std::byte*>(getWorld(registry)) : nullptr;
    if (world == nullptr)
    {
        return Read::NoResult;
    }
    const auto handleFrame = ReadField<std::int32_t>(tracker, kTrackerHandle);
    const auto handleIndex = ReadField<std::int32_t>(tracker, kTrackerHandle + 4);
    if (*g_sweepFlagA != 0 || *g_sweepFlagB != 0)
    {
        // AsyncTrace_GetResultIfIssuedLastFrame: no lock; each hit with a live actor becomes a result {actor}.
        if (handleFrame != ReadField<std::int32_t>(world, kWorldFrame) - 1)
        {
            return Read::NoResult;
        }
        const std::byte* datum = Datum(world, kTraceChunks, kTraceChunkCount, kTraceDatumSize, handleIndex);
        if (datum == nullptr)
        {
            return Read::NoResult;
        }
        const auto* hits  = static_cast<const std::byte*>(ReadPointer(datum, kTraceHits));
        const auto  count = hits != nullptr ? ReadField<std::int32_t>(datum, kTraceHits + 8) : 0;
        if (!Allocate(out, count))
        {
            return Read::NoResult;
        }
        for (std::int32_t i = 0; i < count; ++i)
        {
            const std::byte* weak = hits + static_cast<std::size_t>(kHitSize) * i + kHitActor;
            if (Resolve(weak) != nullptr)
            {
                std::memcpy(out.data + static_cast<std::size_t>(kResultSize) * out.num++, weak, 8);
            }
        }
        return Read::Sweep;
    }
    // The overlap datum, under the lock FieldTick holds while it copies the datum.
    EnterCriticalSection(g_overlapLock);
    bool held = false;
    if (handleFrame == ReadField<std::int32_t>(world, kWorldFrame) - 1)
    {
        if (const std::byte* datum = Datum(world, kOverlapChunks, kOverlapChunkCount, kOverlapDatumSize, handleIndex))
        {
            const auto* results = static_cast<const std::byte*>(ReadPointer(datum, kOverlapResults));
            const auto  count   = results != nullptr ? ReadField<std::int32_t>(datum, kOverlapResults + 8) : 0;
            held                = Allocate(out, count);
            if (held && count > 0)
            {
                // A faulted copy is no result: an empty array would read to the diff as every overlap leaving.
                held    = platform::CopyGuarded(out.data, results, static_cast<std::size_t>(count) * kResultSize);
                out.num = held ? count : 0;
            }
        }
    }
    LeaveCriticalSection(g_overlapLock);
    if (!held && out.data != nullptr)
    {
        g_free(out.data);
        out = {};
    }
    return held ? Read::Overlap : Read::NoResult;
}

/// FieldTick's reset of its scratch structures before a tracker. False (nothing changed) in a state the game resets
/// through a reallocation instead, which it never reaches with its inline allocators.
bool ResetScratch(void* keys, void* set) noexcept
{
    if (ReadField<std::int32_t>(keys, kKeysMax) < 0 || ReadField<std::int32_t>(set, kSetMax) < 0)
    {
        return false;
    }
    WriteField<std::int32_t>(keys, kKeysNum, 0);
    if (ReadField<std::int32_t>(set, kSetNum) != ReadField<std::int32_t>(set, kSetNumFree))
    {
        g_setClear(set);
        WriteField<std::int32_t>(set, kSetNum, 0);
        WriteField<std::int32_t>(set, kSetFirstFree, -1);
        WriteField<std::int32_t>(set, kSetNumFree, 0);
        WriteField<std::int32_t>(set, kSetHashCount, 0);
    }
    return true;
}

void PlayerTwoPass(void* context, void* keys, void* set) noexcept
{
    void* tracker = TrackerOf(generation::PlayerTwoController());
    if (tracker == nullptr || ReadField<std::uint8_t>(tracker, kTrackerEnabled) == 0)
    {
        return;
    }
    ++g_counts.passes;
    if (!ResetScratch(keys, set))
    {
        ++g_counts.skipped;
        if (!g_skipLogged)
        {
            g_skipLogged = true;
            platform::Log(L"trigger two: player 2's pass skipped: the field tick's scratch structures are in a state the "
                          L"game resets by reallocating (logged once)");
        }
        return;
    }
    ResultArray results;
    const Read  read = ReadResult(tracker, results);
    if (read == Read::NoResult)
    {
        ++g_counts.noResult;
        return;
    }
    ++(read == Read::Sweep ? g_counts.sweepReads : g_counts.overlapReads);
    const std::int32_t count = results.num;
    reinterpret_cast<DispatchFn>(g_dispatchCall.Original())(context, tracker, &results, keys, set);
    if (results.data != nullptr)
    {
        g_free(results.data);
    }
    traversal_watch::NoteDispatch(tracker, count);
    if (count != g_lastCount)
    {
        g_lastCount = count;
        platform::LogDebug(L"trigger two: player 2's trigger reads {} overlap(s) ({} result)", count,
                           read == Read::Sweep ? L"sweep" : L"overlap");
    }
}

/// The field tick's dispatch of a tracker. After the game's pass for player 1's tracker, player 2's in the same frame.
std::uintptr_t Dispatch(void* context, void* tracker, void* overlaps, void* keys, void* set) noexcept
{
    // Counted before the diff, which may empty the array.
    const auto count  = ReadField<std::int32_t>(overlaps, 8);
    const auto result = reinterpret_cast<DispatchFn>(g_dispatchCall.Original())(context, tracker, overlaps, keys, set);
    traversal_watch::NoteDispatch(tracker, count);
    if (g_ready && g_filtersPlaced && generation::Active() && generation::OnGameThread() &&
        tracker == TrackerOf(generation::PlayerOneController()) && tracker != TrackerOf(generation::PlayerTwoController()))
    {
        PlayerTwoPass(context, keys, set);
    }
    return result;
}

/// The actor the diff passed, for the log only: the enter's argument points at an overlap result, the leave's is the
/// actor itself.
void* PassedActor(bool enter, void* actorArg) noexcept
{
    if (actorArg == nullptr)
    {
        return nullptr;
    }
    return enter ? Resolve(actorArg) : actorArg;
}

Shadow ShadowOf(std::uint64_t weak) noexcept
{
    const auto found = g_shadow.find(weak);
    return found != g_shadow.end() ? found->second : Shadow{};
}

void KeepShadow(std::uint64_t weak, const Shadow& shadow)
{
    if (shadow.flag == 0)
    {
        g_shadow.erase(weak);
        return;
    }
    if (g_shadow.size() >= kShadowPrune)
    {
        std::erase_if(g_shadow, [](const auto& entry) { return leader_scope::ObjectAt(&entry.first) == nullptr; });
    }
    g_shadow[weak] = shadow;
}

/// The admitted class `actor` belongs to (by its vtable), or -1.
int ClassOf(const void* actor) noexcept
{
    const auto vt = VTableOf(actor);
    for (std::size_t i = 0; i < kClassCount; ++i)
    {
        if (vt != 0 && g_classVt[i] == vt)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LogEvent(bool enter, const void* actor, const void* passed, std::wstring_view outcome)
{
    const wchar_t* match = passed == nullptr ? L"the diff passed none"
                         : passed == actor   ? L"the diff's own"
                                             : L"the diff passed another";
    std::wstring kind = L"no actor";
    if (actor != nullptr)
    {
        const int cls = ClassOf(actor);
        kind          = cls >= 0 ? std::wstring(kAdmitted[cls].name)
                                 : std::format(L"vt 0x{:X}", g_image ? g_image->ResearchAddress(VTableOf(actor)) : 0);
    }
    platform::LogDebug(L"trigger two: player 2's trigger {} {} ({}; {}): {}", enter ? L"enters" : L"leaves",
                       actor != nullptr ? NameOf(actor) : std::wstring(L"(not in the tag registry)"), kind, match,
                       outcome);
}

/// Player 2's event for an admitted actor, run through the game's filter as player 2: the leader and the primary
/// controller answer player 2 (the register and unregister use the leader's controller's list, the enter and leave
/// checks the primary controller's tracker), and the entered flag is player 2's own.
std::uintptr_t Deliver(bool enter, FilterFn original, void* manager, std::uint64_t key, void* owner, void* actorArg,
                       void* actor, const void* passed, const AdmittedClass& cls)
{
    void*      member     = generation::Member();
    const auto memberWeak = leader_scope::WeakOf(member);
    if (member == nullptr || memberWeak == 0 ||
        ReadPointer(member, facts::kPawnController) != generation::PlayerTwoController())
    {
        ++g_counts.refused;
        LogEvent(enter, actor, passed, L"held back: player 2's member is not possessed by player 2's controller");
        return 0;
    }
    const auto actorWeak = leader_scope::WeakOf(actor);
    const auto ownerWeak = leader_scope::WeakOf(owner);
    if (actorWeak == 0 || ownerWeak == 0)
    {
        ++g_counts.refused;
        LogEvent(enter, actor, passed, L"held back: no weak value for the actor or the trigger");
        return 0;
    }
    if (ownerWeak != g_shadowOwner)
    {
        g_shadow.clear();
        g_shadowOwner = ownerWeak;
    }
    const leader_scope::LeaderScope leader(memberWeak);
    const primary_controller::Scope primary;
    if (!leader.Held() || !primary.Held())
    {
        ++g_counts.refused;
        LogEvent(enter, actor, passed,
                 std::format(L"held back: {}", !leader.Held() ? L"the leader scope was refused"
                                                              : primary_controller::Describe(primary.Why())));
        return 0;
    }
    // A nested delivery would take the outer call's swapped-in shadow for player 1's flag and restore it [ASSUMPTION
    // that the game never re-enters the filter from inside it, RE-p2-traversal-L1.md §3.3]: held back instead.
    if (g_delivering)
    {
        ++g_counts.refused;
        LogEvent(enter, actor, passed, L"held back: inside another delivery of player 2's (the filter re-entered)");
        return 0;
    }
    // Player 2's own state in place of player 1's for the call: the entered flag, and the stored character for the
    // classes whose unregister uses it (pole slides, one-shot moves), so player 1's leave never takes the spot off
    // player 2's list and the reverse.
    const Delivering delivering;
    const bool       stores = cls.storedWeak != 0;
    const Shadow real{ReadField<std::uint8_t>(actor, kEnteredFlag),
                      stores ? ReadField<std::uint64_t>(actor, cls.storedWeak) : kNoWeak};
    const Shadow before = ShadowOf(actorWeak);
    const bool   oneShot = cls.logsUseFields;
    const auto   useA = oneShot ? ReadField<std::uint32_t>(actor, kOneShotUseA) : 0u;
    const auto   useB = oneShot ? ReadField<std::int32_t>(actor, kOneShotUseB) : 0;
    WriteField<std::uint8_t>(actor, kEnteredFlag, before.flag);
    if (stores)
    {
        WriteField<std::uint64_t>(actor, cls.storedWeak, before.stored);
    }
    const auto result = original(manager, key, owner, actorArg);
    // The state is read and put back only if the call left the actor alive (no admitted class's enter or leave
    // destroys it; a later one might).
    const bool alive = leader_scope::StillNames(actorWeak, actor);
    Shadow     after;
    if (alive)
    {
        after = {ReadField<std::uint8_t>(actor, kEnteredFlag),
                 stores ? ReadField<std::uint64_t>(actor, cls.storedWeak) : kNoWeak};
        WriteField<std::uint8_t>(actor, kEnteredFlag, real.flag);
        if (stores)
        {
            WriteField<std::uint64_t>(actor, cls.storedWeak, real.stored);
        }
    }
    ++(enter ? g_counts.deliveredEnters : g_counts.deliveredLeaves);
    try
    {
        KeepShadow(actorWeak, after);
        std::wstring extra;
        if (stores)
        {
            extra = std::format(L"; player 2's stored character 0x{:X} -> 0x{:X}, player 1's 0x{:X} kept", before.stored,
                                after.stored, real.stored);
        }
        if (oneShot && alive)
        {
            extra += std::format(L"; +0x848 {} -> {}, +0x86C {} -> {}", useA, ReadField<std::uint32_t>(actor, kOneShotUseA),
                                 useB, ReadField<std::int32_t>(actor, kOneShotUseB));
        }
        LogEvent(enter, actor, passed,
                 std::format(L"delivered as player 2 (player 2's entered flag {} -> {}{}; player 1's {} kept{})",
                             before.flag, after.flag,
                             before.flag == after.flag ? L", unchanged: the game's check stopped it" : L"", real.flag,
                             extra));
    }
    catch (...)
    {
    }
    return result;
}

/// The diff's enter or leave filter. Player 2's trigger's events go to the game as player 2 for admitted classes
/// (kAdmitted), and are held back otherwise.
std::uintptr_t Filter(bool enter, void* manager, std::uint64_t key, void* owner, void* actorArg) noexcept
{
    const auto original = reinterpret_cast<FilterFn>((enter ? g_enterCall : g_leaveCall).Original());
    if (owner == nullptr || !generation::OnGameThread() || !generation::Active() ||
        owner != TriggerOf(generation::PlayerTwoController()))
    {
        return original(manager, key, owner, actorArg);
    }
    ++(enter ? g_counts.enters : g_counts.leaves);
    try
    {
        void*       actor  = g_findActor != nullptr ? g_findActor(&key) : nullptr;
        const void* passed = PassedActor(enter, actorArg);
        const int   cls    = actor != nullptr ? ClassOf(actor) : -1;
        if (cls < 0)
        {
            LogEvent(enter, actor != nullptr ? actor : passed, passed, L"held back, its class is not admitted");
            return 0;
        }
        // A prompt zip line would put its FieldActionMgr point and press flag under both players' notifies (player 1's
        // press could be lost), and player 2 has no prompt there: held back.
        if (kAdmitted[cls].enterEffectOnly && ReadField<std::uint8_t>(actor, kZiplineEnterEffect) == 0)
        {
            LogEvent(enter, actor, passed, L"held back: a button-prompt zip line (only auto-start lines are admitted)");
            return 0;
        }
        return Deliver(enter, original, manager, key, owner, actorArg, actor, passed, kAdmitted[cls]);
    }
    catch (...)
    {
    }
    return 0;
}

std::uintptr_t EnterFilter(void* manager, std::uint64_t key, void* owner, void* actorArg) noexcept
{
    return Filter(true, manager, key, owner, actorArg);
}

std::uintptr_t LeaveFilter(void* manager, std::uint64_t key, void* owner, void* actorArg) noexcept
{
    return Filter(false, manager, key, owner, actorArg);
}

/// The wall-run move state's camera pick. For player 2's LeaderActionComp, the actors on its wall-run list show player
/// 2's entered flags for the call (their real flags are player 1's), so player 2's camera follows player 2's own wall-run
/// volume. Every other call passes through.
void* WallRunPick(void* leaderActionComp) noexcept
{
    const auto original = reinterpret_cast<WallRunPickFn>(g_wallRunCall.Original());
    if (leaderActionComp == nullptr || g_delivering || !generation::Active() || !generation::OnGameThread() ||
        leaderActionComp != ReadPointer(generation::PlayerTwoController(), kControllerLeaderAction))
    {
        return original(leaderActionComp);
    }
    auto*      data  = static_cast<std::byte*>(ReadPointer(leaderActionComp, kWallRunList));
    const auto listed = data != nullptr ? ReadField<std::int32_t>(leaderActionComp, kWallRunList + 8) : 0;
    const auto count  = std::clamp(listed, 0, kWallRunListMax);
    if (listed > kWallRunListMax && !g_wallRunCapLogged)
    {
        g_wallRunCapLogged = true;
        try
        {
            platform::LogDebug(L"trigger two: player 2's wall-run list holds {} actors; past the first {} they keep "
                               L"player 1's entered flags for the camera pick (logged once)",
                               listed, kWallRunListMax);
        }
        catch (...)
        {
        }
    }
    void*         actors[kWallRunListMax] = {};
    std::uint64_t weaks[kWallRunListMax]  = {};
    std::uint8_t  real[kWallRunListMax]   = {};
    for (std::int32_t i = 0; i < count; ++i)
    {
        const std::byte* weak = data + 8 * static_cast<std::size_t>(i);
        actors[i]             = leader_scope::ObjectAt(weak);
        if (actors[i] != nullptr)
        {
            weaks[i] = ReadField<std::uint64_t>(weak, 0);
            real[i]  = ReadField<std::uint8_t>(actors[i], kEnteredFlag);
            WriteField<std::uint8_t>(actors[i], kEnteredFlag, ShadowOf(weaks[i]).flag);
        }
    }
    void* picked = original(leaderActionComp);
    // Put back only on actors the pick left alive (it is a list scan [ASSUMPTION], but a write into a freed actor
    // would crash).
    for (std::int32_t i = 0; i < count; ++i)
    {
        if (actors[i] != nullptr && leader_scope::StillNames(weaks[i], actors[i]))
        {
            WriteField<std::uint8_t>(actors[i], kEnteredFlag, real[i]);
        }
    }
    if (picked != g_wallRunPicked)
    {
        g_wallRunPicked = picked;
        try
        {
            platform::LogDebug(L"trigger two: player 2's wall-run camera pick: {} (of {} wall-run actor(s) near player 2)",
                               picked != nullptr ? NameOf(picked) : std::wstring(L"none"), count);
        }
        catch (...)
        {
        }
    }
    return picked;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto site     = image.Rebase(kDispatchCall, 5);
    const auto dispatch = platform::FindFunction(image, kDispatch);
    const auto enter    = platform::FindFunction(image, kEnterFilter);
    const auto leave    = platform::FindFunction(image, kLeaveFilter);
    const auto enterAt  = image.Rebase(kEnterFilterCall, 5);
    const auto leaveAt  = image.Rebase(kLeaveFilterCall, 5);
    const auto allocate = platform::FindFunction(image, kMalloc);
    const auto release  = platform::FindFunction(image, kFree);
    const auto clear    = platform::FindFunction(image, kSetClear);
    const auto weakGet  = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto registry = image.Rebase(kRegistryWeak, 8);
    const auto lock     = image.Rebase(kOverlapLock, sizeof(CRITICAL_SECTION));
    const auto flagA    = image.Rebase(kSweepFlagA);
    const auto flagB    = image.Rebase(kSweepFlagB);
    const bool found = site && dispatch && enter && leave && enterAt && leaveAt && allocate && release && clear && weakGet &&
                       registry && lock && flagA && flagB;
    if (!found)
    {
        platform::Log(L"trigger two: UNAVAILABLE (a function or datum of the field tick's trigger pass differs); player 2's "
                      L"trigger stays unread, as in the game");
        return;
    }
    g_malloc       = reinterpret_cast<MallocFn>(*allocate);
    g_free         = reinterpret_cast<FreeFn>(*release);
    g_setClear     = reinterpret_cast<SetClearFn>(*clear);
    g_weakGet      = reinterpret_cast<void*>(*weakGet);
    g_registryWeak = reinterpret_cast<const void*>(*registry);
    g_overlapLock  = reinterpret_cast<CRITICAL_SECTION*>(*lock);
    g_sweepFlagA   = reinterpret_cast<const std::uint8_t*>(*flagA);
    g_sweepFlagB   = reinterpret_cast<const std::uint8_t*>(*flagB);
    g_enterSite = *enterAt, g_leaveSite = *leaveAt, g_enterTarget = *enter, g_leaveTarget = *leave;
    // Traversal L1: without the lookup, every event of player 2's stays held back. A class is admitted only if its live
    // vtable's trigger slot holds the shared enter, and (for the classes that store a character) its unregister slot the
    // expected override.
    const auto findActor = platform::FindFunction(image, kFindActor);
    g_findActor          = findActor ? reinterpret_cast<FindActorFn>(*findActor) : nullptr;
    const auto liveEnter = image.Rebase(kTriggerEnter);
    std::wstring admitted, refused;
    for (std::size_t i = 0; i < kClassCount; ++i)
    {
        const AdmittedClass& cls = kAdmitted[i];
        const auto vt            = image.Rebase(cls.vtable, 0x740);
        bool ok = vt && liveEnter && ReadField<std::uintptr_t>(reinterpret_cast<const void*>(*vt), kEnterSlot) == *liveEnter;
        if (ok && cls.unregister != 0)
        {
            const auto unregister = image.Rebase(cls.unregister);
            ok = unregister &&
                 ReadField<std::uintptr_t>(reinterpret_cast<const void*>(*vt), kUnregisterSlot) == *unregister;
        }
        g_classVt[i] = ok ? *vt : 0;
        auto& list   = ok ? admitted : refused;
        list += std::format(L"{}{}", list.empty() ? L"" : L", ", cls.name);
    }
    // The wall-run camera pick (an E8 inside one 16-byte block in both builds).
    const auto pickSite = image.Rebase(kWallRunPickCall, 5);
    const auto pick     = platform::FindFunction(image, kWallRunPick);
    const bool wallRunPick =
        pickSite && pick && g_wallRunCall.Install(*pickSite, *pick, reinterpret_cast<std::uintptr_t>(&WallRunPick));
    platform::Log(L"trigger two: classes delivered as player 2: {}{}{}; wall-run camera pick {}",
                  g_findActor != nullptr ? (admitted.empty() ? L"none" : admitted.c_str()) : L"none (the tag lookup differs)",
                  refused.empty() ? L"" : L"; NOT admitted (a slot differs): ", refused,
                  wallRunPick ? L"follows player 2's own flags" : L"NOT placed (it follows player 1's flags)");
    g_ready = true;
    if (!g_dispatchCall.Install(*site, *dispatch, reinterpret_cast<std::uintptr_t>(&Dispatch)))
    {
        g_ready = false;
        platform::Log(L"trigger two: UNAVAILABLE (the field tick's dispatch call differs); player 2's trigger stays unread");
        return;
    }
    g_filtersPending = true;
    platform::Log(L"trigger two: installed (player 2's trigger is read right after player 1's, in the same frame; the "
                  L"admitted classes go to the game as player 2, every other event is held back; each event logged at "
                  L"Debug; the diff's filter calls follow on the game thread)");
}

void Remove() noexcept
{
    g_filtersPending = false;
    if (g_dispatchCall.IsInstalled() && !g_dispatchCall.Restore())
    {
        platform::Log(L"trigger two: the dispatch call was changed after us; left in place (it passes through)");
    }
    if (g_wallRunCall.IsInstalled() && !g_wallRunCall.Restore())
    {
        platform::Log(L"trigger two: the wall-run pick call was changed after us; left in place (it passes through)");
    }
    // A call crossing two blocks is restored only from the game thread. Elsewhere it stays in place; it holds player 2's
    // events back only while player 2 is active, and passes everything else through.
    if (generation::OnGameThread())
    {
        if ((g_enterCall.IsInstalled() && !g_enterCall.Restore()) | (g_leaveCall.IsInstalled() && !g_leaveCall.Restore()))
        {
            platform::Log(L"trigger two: a filter call was changed after us; left in place (it passes through)");
        }
        g_filtersPlaced = false;
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (g_filtersPending)
    {
        // Here the game thread is outside the field tick and the diff, as InstallFromOwnThread requires.
        g_filtersPending = false;
        const bool enter = g_enterCall.InstallFromOwnThread(g_enterSite, g_enterTarget,
                                                            reinterpret_cast<std::uintptr_t>(&EnterFilter));
        const bool leave = enter && g_leaveCall.InstallFromOwnThread(g_leaveSite, g_leaveTarget,
                                                                     reinterpret_cast<std::uintptr_t>(&LeaveFilter));
        if (enter && !leave)
        {
            g_enterCall.Restore();
        }
        g_filtersPlaced = enter && leave;
        platform::Log(L"trigger two: {}", g_filtersPlaced ? L"the diff's filter calls placed; player 2's pass runs"
                                                          : L"the diff's filter calls differ; player 2's pass stays off");
    }
    if (!generation::Active())
    {
        g_lastCount = -1;
        return;
    }
    const auto clock = std::chrono::steady_clock::now();
    if (clock - g_summaryAt < std::chrono::seconds(10))
    {
        return;
    }
    if (g_summaryAt != std::chrono::steady_clock::time_point{})
    {
        platform::LogDebug(L"trigger two (10 s): player 2's passes {} (overlap results {}, sweep {}, none last frame {}, "
                           L"skipped {}); events: enters {}, leaves {}; delivered as player 2: enters {}, leaves {}; "
                           L"refused by a scope or the member's controller {}; player 2's entered flags held {}",
                           g_counts.passes, g_counts.overlapReads, g_counts.sweepReads, g_counts.noResult,
                           g_counts.skipped, g_counts.enters, g_counts.leaves, g_counts.deliveredEnters,
                           g_counts.deliveredLeaves, g_counts.refused, g_shadow.size());
    }
    g_summaryAt = clock;
    g_counts    = {};
}

}  // namespace lss::player_two::trigger_two
