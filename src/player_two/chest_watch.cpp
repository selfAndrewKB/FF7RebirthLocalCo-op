#include "player_two/chest_watch.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <span>
#include <string>

namespace lss::player_two::chest_watch {

namespace {

// RE-p2-field-interact.md §13.5 (this build).
constexpr std::uintptr_t kChestKey          = 0x1414A8B08;  // TreasurePointMgr_OnFieldActionKey_RequestTreasureState(mgr, &key)
constexpr std::uint8_t   kChestKeyBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10};
constexpr std::uintptr_t kLeaderTestCall    = 0x1434488E7;  // in CharCtrlStateTreasure_OnEnter
constexpr std::uintptr_t kLeaderTest        = 0x143447250;  // CharCtrlStateTreasure_IsCharLeaderOrRide(state)
constexpr std::uintptr_t kGrant             = 0x143444B64;  // CharCtrlStateTreasure_GrantRewards_MarkOpened
constexpr std::uint8_t   kGrantBytes[]      = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::size_t    kFirst             = 5;            // each entry's first instruction: mov [rsp+x], rbx
constexpr std::uint32_t  kPartyLeader       = 0x148;        // the party manager's leader, an FWeakObjectPtr
constexpr std::uint32_t  kStateBits         = 0x131;        // the treasure state's u8 bits; 0x20 granted (§13 listing)
constexpr std::uint8_t   kGrantedBit        = 0x20;
using Clock = std::chrono::steady_clock;
constexpr auto kStalledAfter = std::chrono::seconds(6);   // a press with no grant by then: the walk may be stalled (R-C2)
constexpr auto kWatchFor     = std::chrono::seconds(20);  // an open is followed this long at most

// The chest spawn scan (§14.1, §14.4): CharaPop's selection pass CharaPopObj_ApplySpawnSelection_Despawn9 walks the
// pop object's score-sorted entry pointers and spawns an entry (state 0 -> 1) when its rank (the entries before it without
// flag 1) is under the count budget, its score reaches the memory cut, and its resources are loaded; keep reasons skip
// the rank and score tests. M = [root+0x180], P = [M+0x10] (charapop_facts.h).
constexpr std::uint32_t kSortedInline    = 0x20D8;  // P: entry pointers, inline (0x2000 bytes)
constexpr std::uint32_t kSortedHeap      = 0x40D8;  // P: the same, when on the heap
constexpr std::uint32_t kSortedCount     = 0x40E0;  // P: i32
constexpr std::uint32_t kSpawnMax        = 0x6170;  // P: i32, the spawn budget (field command 317 writes it)
constexpr std::uint32_t kVisibleMax      = 0x6174;  // P: i32, the visible budget
constexpr std::uint32_t kSpawnScale      = 0x6184;  // P: float
constexpr std::uint32_t kManagerSelect   = 0x18;    // M: pointer; the memory cut at +0xE8 (i32)
constexpr std::uint32_t kMemoryCut       = 0xE8;
constexpr std::uint32_t kOverrideOn      = 0x119C;  // M: u8
constexpr std::uint32_t kOverrideCentre  = 0x11A0;  // M: FVector
constexpr std::uint32_t kLeaderCentre    = 0x11B0;  // M: FVector
constexpr std::uint32_t kEntryUseCount   = 0x40;    // i32
constexpr std::uint32_t kEntryScore      = 0x44;    // i32
constexpr std::uint32_t kEntrySpawnPoint = 0x60;    // FVector: the last scored spawn transform's translation
constexpr std::uint32_t kEntryFlags      = 0x118;   // u32: bit 1 not ranked
constexpr std::uint32_t kEntryKeep       = 0x11C;   // u32: keep reasons
constexpr std::uint32_t kEntryHidden     = 0x1A2;   // u8: over the visible budget
constexpr std::int32_t  kMaxSorted       = 8192;
constexpr std::uint64_t kScanEvery       = 30;       // frames
constexpr float         kNearCm          = 8000.0f;  // chests within 80 m of either player
constexpr int           kTracked         = 64;

// Each wrapped function takes at most four integer arguments and no float (§13.5 listings); all four are passed on.
using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

platform::EntryRedirect g_chestKey, g_grant;
platform::CallRedirect  g_leaderTestCall;
std::uintptr_t          g_partyManager = 0;  // the pointer global's live address

// Game thread.
std::uint64_t g_pressFrame = 0;  // the last chest press that started an open

/// The open that press started: its character followed each frame until the grant (R-C2: does it reach the chest).
struct Open
{
    bool              active    = false;
    bool              warned    = false;
    void*             character = nullptr;
    std::uint64_t     weak      = 0;  // the character's weak value: it is read only while it still names it
    Clock::time_point at{};
    float             last[3]{};
    bool              lastKnown = false;
    float             moved     = 0.0f;  // cm along the way
};
Open g_open;

const wchar_t* Whose(const void* character) noexcept
{
    if (character == nullptr)
    {
        return L"nobody";
    }
    if (character == generation::Member())
    {
        return L"player 2's member";
    }
    if (character == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return L"another character (a ride body?)";
}

/// The party leader the game resolves now (PM+0x148).
const void* Leader() noexcept
{
    const void* manager = ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0);
    return manager != nullptr ? leader_scope::ObjectAt(static_cast<const std::uint8_t*>(manager) + kPartyLeader) : nullptr;
}

const wchar_t* PassText() noexcept
{
    return field_pickup::InPass() ? L"in player 2's pass" : L"in player 1's (the game's own) run";
}

std::uint64_t KeyAt(std::uint64_t pointer) noexcept
{
    return pointer != 0 ? ReadField<std::uint64_t>(reinterpret_cast<const void*>(pointer), 0) : 0;
}

std::uint64_t ChestKey(std::uint64_t mgr, std::uint64_t key, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const void* leader = generation::OnGameThread() ? Leader() : nullptr;
    const auto  frame  = platform::frame_watch::FrameIndex();
    const auto  before = g_pressFrame;
    g_pressFrame       = frame;  // before the call: the treasure state's OnEnter runs inside it (§13 R-C1)
    const auto  result = reinterpret_cast<Fn>(g_chestKey.Original())(mgr, key, r8, r9);
    if ((result & 0xFF) == 0)
    {
        g_pressFrame = before;  // no open started
    }
    else if (generation::OnGameThread() && generation::Active())
    {
        g_open       = Open{true, false, const_cast<void*>(leader), 0, Clock::now()};
        g_open.weak      = leader_scope::WeakOf(g_open.character);
        g_open.lastKnown = g_open.weak != 0 && charapop::ActorLocation(g_open.character, g_open.last);
        platform::Log(L"chest watch: frame {}: chest {} pressed {}: the game opens it with {} (the leader it resolved)",
                      g_pressFrame, players::unreal::NameText(KeyAt(key)), PassText(), Whose(leader));
    }
    return result;
}

std::uint64_t LeaderTest(std::uint64_t state, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_leaderTestCall.Original())(state, rdx, r8, r9);
    if (generation::OnGameThread() && generation::Active())
    {
        platform::Log(L"chest watch: frame {}: the treasure state's OnEnter runs {} ({} frame(s) after the press); its "
                      L"leader test says {}: the menu locks {}",
                      platform::frame_watch::FrameIndex(), PassText(), platform::frame_watch::FrameIndex() - g_pressFrame,
                      (result & 0xFF) != 0 ? L"yes" : L"no", (result & 0xFF) != 0 ? L"apply for the open" : L"do not apply");
    }
    return result;
}

/// The grant (rcx = the state). The game also calls it as a no-op (OnExit's guarded call): a grant is logged only when
/// the call set the state's own "granted" bit.
std::uint64_t Grant(std::uint64_t state, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto* bits   = reinterpret_cast<const void*>(state);
    const auto  before = ReadField<std::uint8_t>(bits, kStateBits);
    const auto  result = reinterpret_cast<Fn>(g_grant.Original())(state, b, c, d);
    const auto  after  = ReadField<std::uint8_t>(bits, kStateBits);
    if (generation::OnGameThread() && generation::Active() && (before & kGrantedBit) == 0 && (after & kGrantedBit) != 0)
    {
        const auto frame = platform::frame_watch::FrameIndex();
        platform::Log(L"chest watch: frame {}: the chest's rewards granted {} ({} frame(s) after the press; the opener moved "
                      L"{:.1f} m on the way)",
                      frame, PassText(), frame - g_pressFrame, g_open.active ? g_open.moved / 100.0f : -1.0f);
        g_open.active = false;
    }
    return result;
}

// The chest spawn scan. Game thread.
std::uintptr_t g_gameRoot = 0;  // the game root global's live address

/// What holds a chest that is not spawned, in the selection's own order of tests.
enum class Held
{
    None,    // spawned (or on its way): not held
    NoUse,   // use count 0
    Budget,  // rank at or over the count budget
    Cut,     // score under the memory cut
    Other,   // passes rank and score: its resources are loading, or another gate
};

const wchar_t* HeldText(Held held) noexcept
{
    switch (held)
    {
    case Held::NoUse:  return L", held: no use count";
    case Held::Budget: return L", held: over the count budget";
    case Held::Cut:    return L", held: under the memory cut";
    case Held::Other:  return L", passes rank and score (resources loading, or another gate)";
    default:           return L"";
    }
}

const wchar_t* StateText(std::int32_t state) noexcept
{
    switch (state)
    {
    case 0:  return L"not spawned";
    case 1:
    case 2:
    case 3:  return L"loading";
    case 4:
    case 5:
    case 8:  return L"shown";
    case 6:
    case 7:  return L"spawned hidden (player 1's camera distance, or the visible budget)";
    case 9:
    case 10:
    case 11: return L"despawning";
    default: return L"another state";
    }
}

void NoteChestSpot(std::uint64_t key, const float (&spot)[3]) noexcept;  // the admission tests (below)

/// A chest the scan saw near a player, as last logged.
struct Seen
{
    std::uint64_t key   = 0;
    std::int32_t  state = 0;
    Held          held  = Held::None;
    std::uint64_t scan  = 0;  // the last scan that saw it near; older slots are free
};
std::array<Seen, kTracked> g_seen{};
std::uint64_t              g_scans        = 0;
std::int32_t               g_chestsRanked = -1;  // chest entries in the last scan
const void*                g_sorted[kMaxSorted]{};

/// The slot following `key` (found, or a free one; nullptr when all are taken this scan).
Seen* SlotFor(std::uint64_t key, bool& known) noexcept
{
    Seen* free = nullptr;
    for (auto& seen : g_seen)
    {
        if (seen.key == key && seen.scan + 1 >= g_scans)
        {
            known = true;
            return &seen;
        }
        if (free == nullptr && (seen.scan == 0 || seen.scan + 1 < g_scans))
        {
            free = &seen;
        }
    }
    known = false;
    return free;
}

void Scan() noexcept
{
    const charapop::Objects objects   = charapop::ObjectsOf(g_gameRoot);
    const void*             playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    void*                   member    = generation::Member();
    float                   one[3]{}, two[3]{};
    if (objects.pop == nullptr || playerOne == nullptr || member == nullptr ||
        !charapop::ActorLocation(const_cast<void*>(playerOne), one) || !charapop::ActorLocation(member, two))
    {
        return;
    }
    const auto* heap  = static_cast<const std::byte*>(ReadPointer(objects.pop, kSortedHeap));
    const auto  limit = heap != nullptr ? kMaxSorted : static_cast<std::int32_t>((kSortedHeap - kSortedInline) / sizeof(void*));
    const auto  count = std::clamp(ReadField<std::int32_t>(objects.pop, kSortedCount), 0, limit);
    const auto* list  = heap != nullptr ? heap : objects.pop + kSortedInline;
    if (count == 0 || !platform::CopyGuarded(g_sorted, list, static_cast<std::size_t>(count) * sizeof(void*)))
    {
        return;
    }
    const auto  spawnMax = ReadField<std::int32_t>(objects.pop, kSpawnMax);
    const float scale    = ReadField<float>(objects.pop, kSpawnScale);
    const auto  budget   = static_cast<std::int32_t>(static_cast<float>(spawnMax) * scale);  // the pass's own truncation
    const void* select   = ReadPointer(objects.manager, kManagerSelect);
    const auto  cut      = select != nullptr ? ReadField<std::int32_t>(select, kMemoryCut) : 0;
    float       centre[3]{};
    platform::CopyGuarded(centre, objects.manager + (ReadField<std::uint8_t>(objects.manager, kOverrideOn) != 0 ? kOverrideCentre : kLeaderCentre),
                          sizeof(centre));
    if (g_scans++ == 0)
    {
        platform::Log(L"chest spawn: scan running ({} ranked entries, count budget {} ({} x {:.2f}), visible budget {}, memory cut "
                      L"{}); chests within {:.0f} m of either player are logged when their state changes",
                      count, budget, spawnMax, scale, ReadField<std::int32_t>(objects.pop, kVisibleMax), cut, kNearCm / 100.0f);
    }
    std::int32_t rank = 0, chests = 0, nearestAt = 0;
    const void*  nearest   = nullptr;
    float        nearestCm = 0.0f;
    for (std::int32_t i = 0; i < count; ++i)
    {
        const void* entry = g_sorted[i];
        if (entry == nullptr)
        {
            continue;
        }
        const auto         flags = ReadField<std::uint32_t>(entry, kEntryFlags);
        const std::int32_t at    = rank;
        if ((flags & 1) == 0)
        {
            ++rank;  // the pass counts the entries without flag 1
        }
        float spot[3]{};
        if (!platform::CopyGuarded(spot, static_cast<const std::byte*>(entry) + kEntrySpawnPoint, sizeof(spot)))
        {
            continue;
        }
        const auto key = ReadField<std::uint64_t>(entry, charapop::kEntryKey);
        if (!field_pickup::IsChestEntry(key))
        {
            continue;
        }
        NoteChestSpot(key, spot);  // the admission log's distances
        const float toOne = charapop::Distance(spot, one);
        const float toTwo = charapop::Distance(spot, two);
        ++chests;
        if (nearest == nullptr || toTwo < nearestCm)
        {
            nearest   = entry;
            nearestCm = toTwo;
            nearestAt = at;
        }
        if (toOne > kNearCm && toTwo > kNearCm)
        {
            continue;
        }
        const auto state = ReadField<std::int32_t>(entry, charapop::kEntryState);
        const auto keep  = ReadField<std::uint32_t>(entry, kEntryKeep);
        const auto score = ReadField<std::int32_t>(entry, kEntryScore);
        Held       held  = Held::None;
        if (state == 0)
        {
            held = ReadField<std::int32_t>(entry, kEntryUseCount) <= 0 ? Held::NoUse
                   : keep != 0                                         ? Held::Other  // keep reasons skip rank and score
                   : (flags & 1) == 0 && at >= budget                  ? Held::Budget
                   : score < cut                                       ? Held::Cut
                                                                       : Held::Other;
        }
        bool  known = false;
        Seen* seen  = SlotFor(key, known);
        if (seen == nullptr)
        {
            continue;  // more than kTracked chests near: the rest wait for a slot
        }
        const bool changed = !known || seen->state != state || seen->held != held;
        const auto before  = seen->state;
        *seen              = Seen{key, state, held, g_scans};
        if (!changed)
        {
            continue;
        }
        try
        {
            platform::Log(L"chest spawn: {}: {} (state {}, {}){}: player 1 {:.0f} m, player 2 {:.0f} m, the population's centre "
                          L"{:.0f} m; rank {} / budget {}, score {} / memory cut {}, flags 0x{:X}, keep 0x{:X}, over the visible "
                          L"budget {}",
                          players::unreal::NameText(key), StateText(state), state,
                          known ? std::format(L"was {}", before) : std::wstring(L"newly near"), HeldText(held), toOne / 100.0f,
                          toTwo / 100.0f, charapop::Distance(spot, centre) / 100.0f, at, budget, score, cut, flags, keep,
                          ReadField<std::uint8_t>(entry, kEntryHidden));
        }
        catch (...)
        {
        }
    }
    if (chests != g_chestsRanked)
    {
        g_chestsRanked = chests;  // the field's chest entries (0 also says the keys did not match TreasurePointMgr's)
        try
        {
            platform::Log(L"chest spawn: {} chest entries among {} ranked{}", chests, count,
                          nearest == nullptr
                              ? std::wstring()
                              : std::format(L" (nearest to player 2: {} at {:.0f} m, state {}, rank {})",
                                            players::unreal::NameText(ReadField<std::uint64_t>(nearest, charapop::kEntryKey)),
                                            nearestCm / 100.0f, ReadField<std::int32_t>(nearest, charapop::kEntryState), nearestAt));
        }
        catch (...)
        {
        }
    }
}

// The CharaPop key list's admission tests for chests (RE-p2-world-streaming.md §9.11). The key-list rebuild
// (CharaPopKeyList_Tick_BuildStateMachine, every 5 s) admits a category-1 key within 300 m of the leader only if a ground
// trace down from its spawn point hits "Terrain" (state 3 issues it, state 4 reads it) and the world is ready there (state
// 5: the collision grids). The chest near player 2 comes in only with player 1 within ~88 m: which test holds it is logged.
// A = the key list (the rebuild's rcx).
constexpr std::uintptr_t kBuild           = 0x140877454;  // CharaPopKeyList_Tick_BuildStateMachine(A, float dt)
constexpr std::uint8_t   kBuildBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x10};  // mov [rsp+10h], rbx
constexpr std::uintptr_t kTraceIssueCall  = 0x140877C7C;  // crosses a 16-byte block: installed from the game thread
constexpr std::uintptr_t kTraceIssue      = 0x140A57928;  // (out handle, &start, &end, r9, two stack arguments)
constexpr std::uintptr_t kTraceResultCall = 0x1408777D8;
constexpr std::uintptr_t kTraceResult     = 0x1408876AC;  // AsyncTrace_GetResultIfIssuedLastFrame(&handle, result)
constexpr std::uintptr_t kWorldReadyCall  = 0x14087768D;  // crosses a 16-byte block: installed from the game thread
constexpr std::uintptr_t kWorldReady      = 0x140A2F648;  // EndWorld_IsReadyAt_NavCellAndGrids(&reason, &pos, withNavCell)
constexpr std::uintptr_t kResolveCall     = 0x140877DD7;  // state 2: GS_ResolveObjectIdByName(FName) -> id (0: dropped)
constexpr std::uintptr_t kSlotTestCall    = 0x140877EC8;  // state 2: the slot test's call
constexpr std::uintptr_t kSlotTest        = 0x140878260;  // CharaPopAreaData_IsIdActive_LevelSlotLoaded(areaData, id)
constexpr std::uint32_t  kAllKeys         = 0x8;          // A: every field key (state 1), same layout as the lists below
constexpr std::uint32_t  kListAllIndex    = 0x39290;      // A: i32, the key state 2 is at
constexpr std::uint32_t  kListState       = 0x39284;      // A: i32
constexpr std::uint32_t  kListLeaderAt    = 0x39278;      // A: FVector, the leader's root (the 300 m test's centre)
constexpr std::uint32_t  kListTraceIndex  = 0x39288;      // A: i32, the key state 3 is at
constexpr std::uint32_t  kListReadyIndex  = 0x3928C;      // A: i32, the key state 5 is at
constexpr std::uint32_t  kTracedKeys      = 0x8018;       // A: FName list (inline 0x8000 bytes, heap at +0x8000, count +0x8008)
constexpr std::uint32_t  kReadyKeys       = 0x10028;      // A: the trace's survivors, same layout
constexpr std::uint32_t  kListHeap        = 0x8000;
constexpr std::uint32_t  kListCount       = 0x8008;
constexpr std::int32_t   kListInline      = 0x1000;
constexpr std::uint32_t  kHandles         = 0x30278;      // A: u64 per traced key, the trace handles
constexpr std::uint32_t  kResultHits      = 0x158;        // the trace result: i32 hit count
constexpr float          kLongTraceCm     = 1000.0f;      // a trace longer than this is the "long" one (1500 below, not 600)

using BuildFn       = std::uint64_t (*)(void*, float);
using TraceIssueFn  = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                        std::uint64_t);
using TraceResultFn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using WorldReadyFn  = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
using ResolveFn     = std::uint64_t (*)(std::uint64_t);
using SlotTestFn    = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);

platform::EntryRedirect g_build;
platform::CallRedirect  g_traceIssue, g_traceResult, g_worldReady, g_resolve, g_slotTest;
std::int32_t            g_gateLogged = -1;  // the last logged gate answer: 1 the trace states run, 0 state 2 commits
std::uint64_t           g_round      = 0;   // key-list rounds (state 1 -> 2)
std::uintptr_t          g_traceIssueSite = 0, g_traceIssueTarget = 0, g_worldReadySite = 0, g_worldReadyTarget = 0;
bool                    g_sitesTried     = false;
thread_local const std::byte* g_keyList = nullptr;  // A, while the rebuild runs on this thread
std::mutex          g_candidateLock;           // the candidates, the round and the gate: the rebuild's thread and the scan's
std::atomic<bool>   g_threadLogged{false};
std::atomic<int>    g_rebuildThread{0};        // 0 not seen yet, 1 the game thread, 2 another thread

/// One round's outcome for a chest key.
enum class Admission
{
    Unknown,
    NotCollected,   // not among the field's keys this round (state 1)
    NotResolved,    // its name resolved to no object id: dropped in state 2
    SlotNotActive,  // the slot test (CharaPopAreaData_IsIdActive_LevelSlotLoaded) said no: dropped in state 2
    PassedState2,   // passed state 2, and this field skips the trace states: admitted
    NoTraceResult,  // the trace result was not there (not issued last frame): dropped this round
    TraceMiss,      // the ground trace hit no "Terrain": dropped
    NotReady,       // the trace hit; the world is not ready at the spawn point: dropped
    Admitted,       // the trace hit and the world is ready
};

const wchar_t* AdmissionText(Admission admission) noexcept
{
    switch (admission)
    {
    case Admission::NotCollected:  return L"DROPPED: not among the field's keys (state 1)";
    case Admission::NotResolved:   return L"DROPPED: its name resolves to no object id (state 2)";
    case Admission::SlotNotActive: return L"DROPPED: the slot test says not active (state 2)";
    case Admission::PassedState2:  return L"admitted (passed state 2; this field skips the trace states)";
    case Admission::NoTraceResult: return L"DROPPED: no trace result (not issued last frame)";
    case Admission::TraceMiss:     return L"DROPPED: the ground trace hit no Terrain";
    case Admission::NotReady:      return L"DROPPED: the trace hit, the world is not ready there";
    case Admission::Admitted:      return L"admitted (the trace hit, the world is ready)";
    default:                       return L"unknown";
    }
}

struct Candidate
{
    std::uint64_t key = 0;
    float         at[3]{};  // the spawn point (the trace's start, less its 50 cm lift)
    bool          atKnown   = false;
    bool          longTrace = false;
    std::int32_t  hits      = 0;
    std::uint64_t seenRound = 0;      // the last round state 2 resolved its name
    bool          passed2   = false;  // passed state 2 this round
    Admission     logged    = Admission::Unknown;
    std::uint64_t frame     = 0;  // the last frame it was seen (the oldest slot is reused)
};
std::array<Candidate, kTracked> g_candidates{};

std::uint64_t ListKey(std::uint32_t list, std::int32_t index) noexcept
{
    if (g_keyList == nullptr || index < 0)
    {
        return 0;
    }
    const std::byte* base  = g_keyList + list;
    const auto       count = ReadField<std::int32_t>(base, kListCount);
    const auto*      heap  = static_cast<const std::byte*>(ReadPointer(base, kListHeap));
    if (index >= count || (heap == nullptr && index >= kListInline))
    {
        return 0;
    }
    return ReadField<std::uint64_t>(heap != nullptr ? heap : base, static_cast<std::size_t>(index) * sizeof(std::uint64_t));
}

/// The chest key's slot (found, or the oldest); nullptr for a key that is not a chest's.
Candidate* CandidateFor(std::uint64_t key) noexcept
{
    if (key == 0 || !field_pickup::IsChestEntry(key))
    {
        return nullptr;
    }
    Candidate* oldest = &g_candidates[0];
    for (auto& candidate : g_candidates)
    {
        if (candidate.key == key)
        {
            candidate.frame = platform::frame_watch::FrameIndex();
            return &candidate;
        }
        if (candidate.frame < oldest->frame)
        {
            oldest = &candidate;
        }
    }
    *oldest = Candidate{key};
    oldest->frame = platform::frame_watch::FrameIndex();
    return oldest;
}

/// A round's outcome: Debug each time, Info when it differs from the chest's last one.
void Decide(Candidate& candidate, Admission admission, std::uint8_t reason) noexcept
{
    const bool changed = admission != candidate.logged;
    candidate.logged   = admission;
    if (!changed && !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const void* playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    void*       member    = generation::Member();
    float       one[3]{}, two[3]{}, leader[3]{};
    const bool  oneKnown = playerOne != nullptr && charapop::ActorLocation(const_cast<void*>(playerOne), one);
    const bool  twoKnown = member != nullptr && charapop::ActorLocation(member, two);
    const bool  leaderKnown =
        g_keyList != nullptr && platform::CopyGuarded(leader, g_keyList + kListLeaderAt, sizeof(leader));
    const auto metres = [&](bool known, const float (&from)[3]) {
        return known && candidate.atKnown ? charapop::Distance(candidate.at, from) / 100.0f : -1.0f;
    };
    try
    {
        const auto text = std::format(
            L"chest admission: {}: {}{} ({} trace, {} hit(s)); player 1 {:.0f} m, player 2 {:.0f} m, the list's leader point "
            L"{:.0f} m",
            players::unreal::NameText(candidate.key), AdmissionText(admission),
            admission == Admission::NotReady ? std::format(L", reason {}", reason) : std::wstring(),
            candidate.longTrace ? L"long" : L"short", candidate.hits, metres(oneKnown, one), metres(twoKnown, two),
            metres(leaderKnown, leader));
        changed ? platform::Log(L"{}", text) : platform::LogDebug(L"{}", text);
    }
    catch (...)
    {
    }
}

void NoteChestSpot(std::uint64_t key, const float (&spot)[3]) noexcept
{
    const std::lock_guard lock(g_candidateLock);
    if (Candidate* candidate = CandidateFor(key))
    {
        for (int i = 0; i < 3; ++i)
        {
            candidate->at[i] = spot[i];
        }
        candidate->atKnown = true;
    }
}

// Per round (under g_candidateLock): state 2's calls, for the Debug summary.
std::int32_t g_resolved = 0, g_resolvedChests = 0, g_slotTests = 0, g_slotChests = 0, g_collectedChests = -1;

/// The chest keys among every field key (state 1's list, still whole when state 2 ends) (Debug; Info when their count changes).
void CountCollected() noexcept
{
    const auto   count  = std::min(ReadField<std::int32_t>(g_keyList + kAllKeys, kListCount), kMaxSorted);
    std::int32_t chests = 0;
    std::wstring names;
    for (std::int32_t i = 0; i < count; ++i)
    {
        const auto key = ListKey(kAllKeys, i);
        if (key != 0 && field_pickup::IsChestEntry(key))
        {
            ++chests;
            try
            {
                names += (names.empty() ? L"" : L", ") + players::unreal::NameText(key);
            }
            catch (...)
            {
            }
        }
    }
    const bool changed = chests != g_collectedChests;
    g_collectedChests  = chests;
    if (changed || platform::Enabled(platform::Level::Debug))
    {
        platform::LogAt(changed ? platform::Level::Info : platform::Level::Debug,
                        L"chest admission: round {}: state 1 collected {} field keys, {} of them chests ({})", g_round, count,
                        chests, names);
    }
}

/// State 2 ended: the gate's answer (logged when it changes), and each known chest's round outcome where state 2 decides
/// it (not collected; admitted when the trace states are skipped).
void EndState2(std::int32_t next) noexcept
{
    const std::int32_t traces = next == 3 ? 1 : 0;
    CountCollected();
    platform::LogDebug(L"chest admission: round {}: state 2 resolved {} names ({} chests), ran the slot test {} times ({} "
                       L"chests); next state {}",
                       g_round, g_resolved, g_resolvedChests, g_slotTests, g_slotChests, next);
    if (traces != g_gateLogged)
    {
        g_gateLogged = traces;
        platform::Log(L"chest admission: this field {} (the field row's +0x148 and the root phase over 9; RE §9.11)",
                      traces != 0 ? L"runs the trace states (ground trace, world-ready) after state 2"
                                  : L"skips the trace states: state 2's survivors are committed");
    }
    for (auto& candidate : g_candidates)
    {
        if (candidate.key == 0)
        {
            continue;
        }
        if (candidate.seenRound != g_round)
        {
            Decide(candidate, Admission::NotCollected, 0);
        }
        else if (candidate.passed2 && traces == 0)
        {
            Decide(candidate, Admission::PassedState2, 0);
        }
    }
    g_resolved = g_resolvedChests = g_slotTests = g_slotChests = 0;
}

std::uint64_t Build(void* list, float dt) noexcept
{
    if (!g_threadLogged.exchange(true))
    {
        const bool game = generation::OnGameThread();
        g_rebuildThread.store(game ? 1 : 2);
        platform::Log(L"chest admission: the key-list rebuild runs {}", game ? L"on the game thread" : L"on ANOTHER thread "
                      L"(not the game thread): the hooks follow it there; the two crossing calls stay unwatched");
    }
    const bool watch = generation::Active() && list != nullptr;
    const auto state = watch ? ReadField<std::int32_t>(list, kListState) : -1;
    if (watch)
    {
        g_keyList = static_cast<const std::byte*>(list);
    }
    const auto result = reinterpret_cast<BuildFn>(g_build.Original())(list, dt);
    if (watch)
    {
        const std::lock_guard lock(g_candidateLock);
        const auto            next = ReadField<std::int32_t>(list, kListState);
        if (state == 1 && next == 2)
        {
            ++g_round;
            for (auto& candidate : g_candidates)
            {
                candidate.passed2 = false;
            }
        }
        else if (state == 2 && next != 2)
        {
            EndState2(next);
        }
    }
    g_keyList = nullptr;
    return result;
}

std::uint64_t Resolve(std::uint64_t key) noexcept
{
    const auto id = reinterpret_cast<ResolveFn>(g_resolve.Original())(key);
    if (g_keyList != nullptr)
    {
        const std::lock_guard lock(g_candidateLock);
        ++g_resolved;
        if (Candidate* candidate = CandidateFor(key))
        {
            ++g_resolvedChests;
            candidate->seenRound = g_round;
            if ((id & 0xFFFFFFFF) == 0)
            {
                Decide(*candidate, Admission::NotResolved, 0);
            }
        }
    }
    return id;
}

std::uint64_t SlotTest(std::uint64_t areaData, std::uint64_t id, std::uint64_t r8) noexcept
{
    const auto active = reinterpret_cast<SlotTestFn>(g_slotTest.Original())(areaData, id, r8);
    if (g_keyList != nullptr)
    {
        const auto            key = ListKey(kAllKeys, ReadField<std::int32_t>(g_keyList, kListAllIndex));
        const std::lock_guard lock(g_candidateLock);
        ++g_slotTests;
        if (Candidate* candidate = CandidateFor(key))
        {
            ++g_slotChests;
            if ((active & 0xFF) == 0)
            {
                Decide(*candidate, Admission::SlotNotActive, 0);
            }
            else
            {
                candidate->passed2 = true;
                platform::LogDebug(L"chest admission: {}: passed state 2 (id {})", players::unreal::NameText(key),
                                   id & 0xFFFFFFFF);
            }
        }
    }
    return active;
}

std::uint64_t TraceIssue(std::uint64_t handle, std::uint64_t start, std::uint64_t end, std::uint64_t r9, std::uint64_t s5,
                         std::uint64_t s6) noexcept
{
    if (g_keyList != nullptr)
    {
        const auto            key = ListKey(kTracedKeys, ReadField<std::int32_t>(g_keyList, kListTraceIndex));
        const std::lock_guard lock(g_candidateLock);
        if (Candidate* candidate = CandidateFor(key))
        {
            float from[3]{}, to[3]{};
            candidate->atKnown = platform::CopyGuarded(from, reinterpret_cast<const void*>(start), sizeof(from)) &&
                                 platform::CopyGuarded(to, reinterpret_cast<const void*>(end), sizeof(to));
            if (candidate->atKnown)
            {
                candidate->at[0]     = from[0];
                candidate->at[1]     = from[1];
                candidate->at[2]     = from[2] - 50.0f;
                candidate->longTrace = from[2] - to[2] > kLongTraceCm;
            }
            platform::LogDebug(L"chest admission: {}: ground trace issued ({} trace)", players::unreal::NameText(key),
                               candidate->longTrace ? L"long" : L"short");
        }
    }
    return reinterpret_cast<TraceIssueFn>(g_traceIssue.Original())(handle, start, end, r9, s5, s6);
}

std::uint64_t TraceResult(std::uint64_t handle, std::uint64_t result) noexcept
{
    const auto ok = reinterpret_cast<TraceResultFn>(g_traceResult.Original())(handle, result);
    if (g_keyList != nullptr)
    {
        const auto first = reinterpret_cast<std::uintptr_t>(g_keyList) + kHandles;
        const auto index = handle >= first ? static_cast<std::int32_t>((handle - first) / sizeof(std::uint64_t)) : -1;
        const std::lock_guard lock(g_candidateLock);
        if (Candidate* candidate = CandidateFor(ListKey(kTracedKeys, index)))
        {
            candidate->hits = (ok & 0xFF) != 0 ? ReadField<std::int32_t>(reinterpret_cast<const void*>(result), kResultHits) : 0;
            if ((ok & 0xFF) == 0)
            {
                Decide(*candidate, Admission::NoTraceResult, 0);
            }
            else if (candidate->hits <= 0)
            {
                Decide(*candidate, Admission::TraceMiss, 0);
            }
            else
            {
                platform::LogDebug(L"chest admission: {}: the ground trace hit ({} hit(s))",
                                   players::unreal::NameText(candidate->key), candidate->hits);
            }
        }
    }
    return ok;
}

std::uint64_t WorldReady(std::uint64_t reason, std::uint64_t at, std::uint64_t withNavCell) noexcept
{
    const auto ready = reinterpret_cast<WorldReadyFn>(g_worldReady.Original())(reason, at, withNavCell);
    if (g_keyList != nullptr)
    {
        const auto            key = ListKey(kReadyKeys, ReadField<std::int32_t>(g_keyList, kListReadyIndex));
        const std::lock_guard lock(g_candidateLock);
        if (Candidate* candidate = CandidateFor(key))
        {
            Decide(*candidate, (ready & 0xFF) != 0 ? Admission::Admitted : Admission::NotReady,
                   ReadField<std::uint8_t>(reinterpret_cast<const void*>(reason), 0));
        }
    }
    return ready;
}

/// The two call sites that cross a 16-byte block, from the game thread outside the rebuild (InstallFromOwnThread).
void InstallCrossingSites() noexcept
{
    // Only once the rebuild was seen on the game thread: InstallFromOwnThread is safe only from the one thread running them.
    if (g_sitesTried || g_traceIssueSite == 0 || g_worldReadySite == 0 || g_rebuildThread.load() != 1)
    {
        return;
    }
    g_sitesTried     = true;
    const bool issue = g_traceIssue.InstallFromOwnThread(g_traceIssueSite, g_traceIssueTarget,
                                                         reinterpret_cast<std::uintptr_t>(&TraceIssue));
    const bool ready = g_worldReady.InstallFromOwnThread(g_worldReadySite, g_worldReadyTarget,
                                                         reinterpret_cast<std::uintptr_t>(&WorldReady));
    platform::Log(L"chest watch (logging only): chest admission tests: the ground trace's issue {}, its result {}, the "
                  L"world-ready test {}; each chest's outcome is logged when it changes (every round at Debug)",
                  issue ? L"watched" : L"NOT watched", g_traceResult.IsInstalled() ? L"watched" : L"NOT watched",
                  ready ? L"watched" : L"NOT watched");
}

// The chests' real spawn gate (RE-p2-world-streaming.md §9.12): WorldActorMgr (*(root+0x190)) holds an item per chest that
// TreasurePointMgr registered. Each frame its tick (WorldActorMgr_Tick_LeaderProximitySpawn) requests a type-2 CharaPop
// spawn for an armed item (state 6) whose octree box (location +- E) overlaps the party leader's bounding box and whose
// location is within the item's pop radius R of the leader's box centre, and removes a spawned one beyond R. Read only,
// on the game thread (the tick's thread): each chest item near either player is logged when its state changes.
constexpr std::uint32_t kRootWorldActors  = 0x190;
constexpr std::uint32_t kItemsData        = 0x20;   // mgr: the item TSet's sparse array data
constexpr std::uint32_t kItemsNum         = 0x28;   // mgr: i32, its ArrayNum
constexpr std::uint32_t kItemsBitsInline  = 0x30;   // mgr: the allocation bits, inline (4 dwords)
constexpr std::uint32_t kItemsBitsHeap    = 0x40;   // mgr: the allocation bits, on the heap
constexpr std::uint32_t kItemsRequested   = 0xC8;   // mgr: i32, the requested keys' count
constexpr std::size_t   kItemElement      = 0x120;  // element: key FName +0, the item at +0x10
constexpr std::size_t   kItemAt           = 0x10;
constexpr std::uint32_t kItemOctree       = 0x08;   // i32, -1 not inserted
constexpr std::uint32_t kItemRadius       = 0x18;   // float R, the pop radius
constexpr std::uint32_t kItemState        = 0x1C;   // i32
constexpr std::uint32_t kItemExtent       = 0x20;   // float E, the octree half-extent
constexpr std::uint32_t kItemKey          = 0x30;   // FName, the CharaPop key
constexpr std::uint32_t kItemLocation     = 0x50;   // FVector
constexpr std::int32_t  kItemsMax         = 4096;
constexpr std::uint64_t kItemScanEvery    = 5;          // frames
constexpr float         kItemNearCm       = 15000.0f;   // chests within 150 m of either player

const wchar_t* ItemStateText(std::int32_t state) noexcept
{
    switch (state)
    {
    case 6:  return L"armed";
    case 0:  return L"spawn requested";
    case 1:  return L"spawned";
    case 4:  return L"despawned by CharaPop while requested";
    case 2:  return L"removal requested";
    case 3:  return L"disabled";
    case 5:  return L"waiting for the field octree";
    default: return L"another state";
    }
}

struct ItemSeen
{
    std::uint64_t key   = 0;
    std::int32_t  state = -1;
    std::uint64_t scan  = 0;
};
std::array<ItemSeen, kTracked> g_itemsSeen{};
std::uint64_t                  g_itemScans  = 0;
std::int32_t                   g_chestItems = -1;

void ItemScan() noexcept
{
    const void* root      = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    const auto* mgr       = static_cast<const std::byte*>(ReadPointer(root, kRootWorldActors));
    const void* playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    void*       member    = generation::Member();
    float       one[3]{}, two[3]{};
    if (mgr == nullptr || playerOne == nullptr || member == nullptr ||
        !charapop::ActorLocation(const_cast<void*>(playerOne), one) || !charapop::ActorLocation(member, two))
    {
        return;
    }
    const auto* data     = static_cast<const std::byte*>(ReadPointer(mgr, kItemsData));
    const auto  num      = std::clamp(ReadField<std::int32_t>(mgr, kItemsNum), 0, kItemsMax);
    const auto* bitsHeap = static_cast<const std::byte*>(ReadPointer(mgr, kItemsBitsHeap));
    const auto* bits     = bitsHeap != nullptr ? bitsHeap : mgr + kItemsBitsInline;
    if (data == nullptr)
    {
        return;
    }
    ++g_itemScans;
    std::int32_t chests = 0;
    float        radiusMin = 0.0f, radiusMax = 0.0f, extentMin = 0.0f, extentMax = 0.0f;
    for (std::int32_t i = 0; i < num; ++i)
    {
        if (bitsHeap == nullptr && i >= 128)
        {
            break;  // the inline bits hold 128
        }
        const auto word = ReadField<std::uint32_t>(bits, static_cast<std::size_t>(i / 32) * sizeof(std::uint32_t));
        if ((word >> (i % 32) & 1) == 0)
        {
            continue;  // a free slot
        }
        const std::byte* item = data + static_cast<std::size_t>(i) * kItemElement + kItemAt;
        const auto       key  = ReadField<std::uint64_t>(item, kItemKey);
        if (key == 0 || !field_pickup::IsChestEntry(key))
        {
            continue;
        }
        float at[3]{};
        if (!platform::CopyGuarded(at, item + kItemLocation, sizeof(at)))
        {
            continue;
        }
        const float radius = ReadField<float>(item, kItemRadius);
        const float extent = ReadField<float>(item, kItemExtent);
        radiusMin          = chests == 0 ? radius : std::min(radiusMin, radius);
        radiusMax          = chests == 0 ? radius : std::max(radiusMax, radius);
        extentMin          = chests == 0 ? extent : std::min(extentMin, extent);
        extentMax          = chests == 0 ? extent : std::max(extentMax, extent);
        ++chests;
        const float toOne = charapop::Distance(at, one);
        const float toTwo = charapop::Distance(at, two);
        if (toOne > kItemNearCm && toTwo > kItemNearCm)
        {
            continue;
        }
        const auto state = ReadField<std::int32_t>(item, kItemState);
        ItemSeen*  slot  = nullptr;
        ItemSeen*  free  = nullptr;
        for (auto& seen : g_itemsSeen)
        {
            if (seen.key == key)
            {
                slot = &seen;
                break;
            }
            if (free == nullptr && (seen.key == 0 || seen.scan + 2 < g_itemScans))
            {
                free = &seen;
            }
        }
        const bool known  = slot != nullptr && slot->scan + 1 >= g_itemScans;
        const auto before = known ? slot->state : -1;
        if (slot == nullptr)
        {
            slot = free;
        }
        if (slot == nullptr)
        {
            continue;  // more than kTracked chests near
        }
        *slot = ItemSeen{key, state, g_itemScans};
        if (known && before == state)
        {
            continue;
        }
        try
        {
            platform::Log(L"chest world actor: {}: {} (state {}{}); pop radius {:.1f} m, octree half-extent {:.1f} m, octree "
                          L"id {}; player 1 {:.1f} m, player 2 {:.1f} m",
                          players::unreal::NameText(key), ItemStateText(state), state,
                          known ? std::format(L", was {}", before) : std::wstring(L", newly near"), radius / 100.0f,
                          extent / 100.0f, ReadField<std::int32_t>(item, kItemOctree), toOne / 100.0f, toTwo / 100.0f);
        }
        catch (...)
        {
        }
    }
    if (chests != g_chestItems)
    {
        g_chestItems = chests;
        platform::Log(L"chest world actor: {} chest items among {} world-actor slots (pop radius {:.1f}-{:.1f} m, octree "
                      L"half-extent {:.1f}-{:.1f} m), {} requested",
                      chests, num, radiusMin / 100.0f, radiusMax / 100.0f, extentMin / 100.0f, extentMax / 100.0f,
                      ReadField<std::int32_t>(mgr, kItemsRequested));
    }
}

/// Follows an open from its press to its grant (R-C2).
void FollowOpen() noexcept
{
    if (!g_open.active)
    {
        return;
    }
    if (g_open.weak == 0 || !leader_scope::StillNames(g_open.weak, g_open.character))
    {
        g_open.active = false;  // the opener is gone
        return;
    }
    const auto since = Clock::now() - g_open.at;
    float      now[3]{};
    if (charapop::ActorLocation(g_open.character, now))
    {
        g_open.moved += g_open.lastKnown ? charapop::Distance(now, g_open.last) : 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            g_open.last[i] = now[i];
        }
        g_open.lastKnown = true;
        platform::LogTrace(L"chest watch: frame {}: the opener at ({:.0f}, {:.0f}, {:.0f}), {:.1f} m moved since the press",
                           platform::frame_watch::FrameIndex(), now[0], now[1], now[2], g_open.moved / 100.0f);
    }
    if (since >= kStalledAfter && !g_open.warned)
    {
        g_open.warned = true;
        platform::Log(L"chest watch: WARNING: no grant {} s after the press; the opener ({}) moved {:.1f} m: the walk to the "
                      L"chest may be stalled",
                      std::chrono::duration_cast<std::chrono::seconds>(kStalledAfter).count(), Whose(g_open.character),
                      g_open.moved / 100.0f);
    }
    if (since >= kWatchFor)
    {
        g_open.active = false;
    }
}

}  // namespace

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    InstallCrossingSites();
    if (g_gameRoot != 0 && generation::Active() && platform::frame_watch::FrameIndex() % kScanEvery == 0)
    {
        Scan();
    }
    if (g_gameRoot != 0 && generation::Active() && platform::frame_watch::FrameIndex() % kItemScanEvery == 0)
    {
        ItemScan();
    }
    FollowOpen();
}

namespace {

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, std::uintptr_t address,
           std::span<const std::uint8_t> bytes, Fn replacement) noexcept
{
    const auto at = image.Rebase(address);
    return at && image.BytesMatch(address, bytes) &&
           redirect.Install(*at, bytes.first(kFirst), reinterpret_cast<std::uintptr_t>(replacement));
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    g_partyManager     = manager ? *manager : 0;
    const bool chests  = g_partyManager != 0 && Entry(image, g_chestKey, kChestKey, kChestKeyBytes, &ChestKey);
    const bool grants  = Entry(image, g_grant, kGrant, kGrantBytes, &Grant);
    const auto site    = image.Rebase(kLeaderTestCall, 5);
    const auto test    = image.Rebase(kLeaderTest);
    const bool tests   = site && test && g_leaderTestCall.Install(*site, *test, reinterpret_cast<std::uintptr_t>(&LeaderTest));
    const auto root    = image.Rebase(charapop::kGameRoot, sizeof(void*));
    g_gameRoot         = root ? *root : 0;
    // The chest admission tests: the rebuild's entry and the trace result's call now (inside one block); the two crossing
    // calls from the first game-thread tick.
    const auto build       = image.Rebase(kBuild);
    const bool builds      = build && image.BytesMatch(kBuild, kBuildBytes) &&
                        g_build.Install(*build, kBuildBytes, reinterpret_cast<std::uintptr_t>(&Build));
    // State 2's id lookup: its target read from the call itself (E8 rel32), and the same as the state-5 lookup's.
    const auto resolveSite = image.Rebase(kResolveCall, 5);
    const auto slotSite    = image.Rebase(kSlotTestCall, 5);
    const auto slotFn      = image.Rebase(kSlotTest);
    std::uintptr_t resolveTarget = 0;
    if (resolveSite && ReadField<std::uint8_t>(reinterpret_cast<const void*>(*resolveSite), 0) == 0xE8)
    {
        resolveTarget = *resolveSite + 5 +
                        static_cast<std::intptr_t>(ReadField<std::int32_t>(reinterpret_cast<const void*>(*resolveSite), 1));
    }
    const bool resolves = builds && resolveTarget != 0 &&
                          g_resolve.Install(*resolveSite, resolveTarget, reinterpret_cast<std::uintptr_t>(&Resolve));
    const bool slots    = builds && slotSite && slotFn &&
                       g_slotTest.Install(*slotSite, *slotFn, reinterpret_cast<std::uintptr_t>(&SlotTest));
    platform::Log(L"chest watch (logging only): chest admission, state 2: the id lookup {}, the slot test {}",
                  resolves ? L"watched" : L"NOT watched", slots ? L"watched" : L"NOT watched");
    const auto resultSite  = image.Rebase(kTraceResultCall, 5);
    const auto resultFn    = image.Rebase(kTraceResult);
    const bool results     = builds && resultSite && resultFn &&
                         g_traceResult.Install(*resultSite, *resultFn, reinterpret_cast<std::uintptr_t>(&TraceResult));
    const auto issueSite   = image.Rebase(kTraceIssueCall, 5);
    const auto issueFn     = image.Rebase(kTraceIssue);
    const auto readySite   = image.Rebase(kWorldReadyCall, 5);
    const auto readyFn     = image.Rebase(kWorldReady);
    if (builds && issueSite && issueFn && readySite && readyFn)
    {
        g_traceIssueSite   = *issueSite;
        g_traceIssueTarget = *issueFn;
        g_worldReadySite   = *readySite;
        g_worldReadyTarget = *readyFn;
    }
    else
    {
        platform::Log(L"chest watch (logging only): chest admission tests NOT watched (the key-list rebuild {}, the trace "
                      L"result {})",
                      builds ? L"watched" : L"NOT watched", results ? L"watched" : L"NOT watched");
    }
    platform::Log(L"chest watch (logging only): chest presses {}, the open's leader test {}, grants {}, "
                  L"chest spawns {}",
                  chests ? L"watched" : L"NOT watched", tests ? L"watched" : L"NOT watched",
                  grants ? L"watched" : L"NOT watched", g_gameRoot != 0 ? L"scanned" : L"NOT scanned");
}

}  // namespace lss::player_two::chest_watch
