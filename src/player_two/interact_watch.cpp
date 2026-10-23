#include "player_two/interact_watch.h"

#include "facts/field_action.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "player_two/rumble_watch.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <intrin.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::interact_watch {

namespace {

// The objects (RE-p2-field-interact.md §1.1, §10; this build).
using facts::kEndGameRoot;
using facts::kRootWorldItems;
using facts::kPartyManagerGlobal;
using facts::kPartyManagerPrimaryController;
using facts::kPartyManagerLeader;
using facts::kPartyManagerLeaderType;

// FieldActionMgr (§1.2, §1.3, §10.D).
constexpr std::uint32_t kOctree         = 0x10;
constexpr std::uint32_t kCandidateCount = 0x298;    // i32
using facts::kFieldActionPointList;
using facts::kFieldActionNodeNext;
using facts::kFieldActionNodePoint;
using facts::kFieldActionPointLock;
using facts::kFieldActionSlotRows;
constexpr std::uint32_t kSlotRecords    = 0x484A8;  // 8 x 36 bytes: +0 key, +8 owner, +0x18 hold timer
constexpr std::uint32_t kSlotRecordSize = 36;
using facts::kFieldActionSlotTargets;
using facts::kFieldActionSlotHeld;
using facts::kFieldActionSlotPressed;
using facts::kFieldActionSlotExecuted;
using facts::kFieldActionPending;
using facts::kFieldActionSlots;
using facts::kFieldActionMaxPoints;

// A point (§1.2 as corrected in §10.B).
constexpr std::uint32_t kPointTalk          = 0x08;   // u8
using facts::kFieldActionPointKey;
constexpr std::uint32_t kPointOwner         = 0x1C;   // FName
constexpr std::uint32_t kPointLocation      = 0x2C;   // FVector
using facts::kFieldActionPointPreset;
constexpr std::uint32_t kPointPreset2       = 0x4C;   // FName, a second point-side preset
constexpr std::uint32_t kPointPresetPlayer  = 0x54;   // FName rpa<Owner>Player
constexpr std::uint32_t kPointSlot          = 0xE8;   // u8
constexpr std::uint32_t kPointKind          = 0xF0;   // u8
constexpr std::uint32_t kPointVariant       = 0xF1;   // u8
using facts::kFieldActionPointFlags;

// WorldItemMgr's map of items (§10.E): a stock TMap keyed by the item's FName at element +0 (facts/world.h).
using facts::kWorldItemMapElements;
using facts::kWorldItemMapStride;
constexpr std::int32_t  kMaxItems       = 16384;
using facts::kFieldActionEntryKeyBack;

// The hooked functions (§10.A). Entry redirects replace the first instruction (5 bytes); the rest of the
// listed bytes are checked too.
constexpr std::uint8_t   kAddPointBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C,
                                             0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
// void(mgr, talk, key*, owner*, u64*, loc*, rot*, preset*, presetPlayer*, float, kind, u8, u8, FName, FName)
constexpr platform::FunctionFact kAddPoint{L"FieldActionMgr's AddPoint", 0x1411BDE60, kAddPointBytes};
constexpr std::uint8_t   kExecuteSlotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// void(mgr, u8 slot)
constexpr platform::FunctionFact kExecuteSlot{L"FieldActionMgr's ExecuteSlot", 0x1414A90E4, kExecuteSlotBytes};
constexpr std::size_t    kFirstInstruction = 5;
// 1.005 (RE-1005-field-2.md §5): AddPoint's entry is at 0x140FED95C, %16 = 12, so its first instruction crosses a
// 16-byte block. Its second (mov [rsp+20h], r9 at %16 = 1) takes the redirect instead: the first only stores rbx into
// the caller's home area and nothing branches to the second, so the replacement is entered as at the entry, with every
// caller's registration, on any thread (the trampoline replays both instructions).
constexpr std::size_t    kSecondInstruction = 5;
// The Wheelie ride-on point's key (an FName global, RE-p2-wheelie-ride.md §11): no analysed code adds it, so its
// registration is logged at Info to name the registrar.
constexpr std::uintptr_t kScooterKey = 0x1491E6D10;
constexpr std::uintptr_t kMarkFromListener = 0x1414A90AD;  // the WorldItem key listener's call returns here
// The candidate query, called read-only for the dry run (§10.C): void(octree, node, context, box*, cap*).
constexpr std::uint8_t   kQueryBytes[] = {0x41, 0x55, 0x41, 0x56, 0xB8, 0x28, 0x01, 0x00, 0x00};
constexpr platform::FunctionFact kQuery{L"the octree's candidate query", 0x1422BA490, kQueryBytes};
constexpr float          kQueryExtent  = 10.0f;  // as the update's call (cm)

constexpr float     kNearCm         = 2000.0f;  // the census: points within 20 m of either player
constexpr ULONGLONG kCensusPeriodMs = 500;
constexpr ULONGLONG kRegistrarPeriodMs = 30'000;

using AddPointFn = void (*)(void* mgr, std::uint8_t talk, const std::uint64_t* key, const std::uint64_t* owner,
                            const std::uint64_t* extra, const float* location, const float* rotation,
                            const std::uint64_t* preset, const std::uint64_t* presetPlayer, float hold,
                            std::uint8_t kind, std::uint8_t flagA, std::uint8_t flagB, std::uint64_t preset2,
                            std::uint64_t presetPlayer2);
using ExecuteSlotFn = void (*)(void* mgr, std::uint8_t slot);
using QueryFn       = void (*)(void* octree, std::int32_t node, void* context, const float* box, void* cap);

struct RawArray  // TArray<Point*> with the default allocator
{
    void**       data;
    std::int32_t num;
    std::int32_t max;
};

platform::EntryRedirect g_addPoint;
platform::EntryRedirect g_executeSlot;
QueryFn                 g_query     = nullptr;
std::optional<platform::GameImage> g_image;  // for research IDA addresses in the log
std::uintptr_t          g_root      = 0;  // live address of the root global
std::uintptr_t          g_party     = 0;  // live address of the party manager global
const std::uint64_t*    g_scooterKey = nullptr;  // live address of the ride-on key's FName global

// Registrations (any thread): logged, and counted per registrar for the summary.
SRWLOCK                                     g_addLock = SRWLOCK_INIT;
std::unordered_map<std::uintptr_t, std::uint64_t> g_addsByRegistrar;  // IDA return address -> count since summary
thread_local int                            t_marksInExecute = -1;  // -1 outside ExecuteSlot
thread_local std::int32_t                   t_effectsBefore[2] = {0, 0};  // each controller's effects before a grant

// Game thread (the update hook).
std::array<std::uint8_t, 8 * kFieldActionSlots + kFieldActionSlots * 8 + 3 * kFieldActionSlots + 1> g_slotSeen{};
ULONGLONG                                g_censusAt = 0;
ULONGLONG                                g_registrarsAt = 0;
ULONGLONG                                g_censusSummaryAt = 0;
struct PointSeen
{
    std::uint32_t flags = 0;
    std::uint8_t  slot = 0, talk = 0;
    bool          nearOne = false, nearTwo = false;
    bool operator==(const PointSeen&) const = default;
};
std::unordered_map<std::uint64_t, PointSeen> g_points;  // key -> last logged
std::vector<std::uint64_t>                   g_dryRunSeen;
std::array<void*, kFieldActionMaxPoints>                g_found{};
bool                                         g_wasIn = false;

class Lock
{
public:
    explicit Lock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~Lock() { ReleaseSRWLockExclusive(&lock_); }
    Lock(const Lock&)            = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SRWLOCK& lock_;
};

/// The game's own point lock, shared, for one census walk (released on every path out).
class SharedLock
{
public:
    explicit SharedLock(PSRWLOCK lock) noexcept : lock_(lock) { AcquireSRWLockShared(lock_); }
    ~SharedLock() { ReleaseSRWLockShared(lock_); }
    SharedLock(const SharedLock&)            = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    PSRWLOCK lock_;
};

const std::uint8_t* PointerAt(const void* base, std::size_t offset) noexcept
{
    return static_cast<const std::uint8_t*>(ReadPointer(base, offset));
}

std::uintptr_t Ida(std::uintptr_t live) noexcept
{
    return g_image ? g_image->ResearchAddress(live) : live;
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"-") : players::unreal::NameText(name);
}

const std::uint8_t* Root() noexcept
{
    return PointerAt(reinterpret_cast<const void*>(g_root), 0);
}

const std::uint8_t* Party() noexcept
{
    return PointerAt(reinterpret_cast<const void*>(g_party), 0);
}

void* Leader() noexcept
{
    const auto* party = Party();
    return party != nullptr ? leader_scope::ObjectAt(party + kPartyManagerLeader) : nullptr;
}

void* PrimaryController() noexcept
{
    const auto* party = Party();
    return party != nullptr ? leader_scope::ObjectAt(party + kPartyManagerPrimaryController) : nullptr;
}

/// The leader and primary controller for the log: resolved only on the game thread (the weak-pointer resolve is
/// game-thread code).
std::wstring LeaderText()
{
    return generation::OnGameThread() ? Who(Leader()) : std::wstring(L"(off the game thread)");
}

std::wstring PrimaryText()
{
    return generation::OnGameThread() ? Who(PrimaryController()) : std::wstring(L"(off the game thread)");
}

float Distance(const float (&a)[3], const float (&b)[3]) noexcept
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// --- Hooks ------------------------------------------------------------------------------------------------

void AddPoint(void* mgr, std::uint8_t talk, const std::uint64_t* key, const std::uint64_t* owner,
              const std::uint64_t* extra, const float* location, const float* rotation, const std::uint64_t* preset,
              const std::uint64_t* presetPlayer, float hold, std::uint8_t kind, std::uint8_t flagA,
              std::uint8_t flagB, std::uint64_t preset2, std::uint64_t presetPlayer2) noexcept
{
    const auto registrar = Ida(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    reinterpret_cast<AddPointFn>(g_addPoint.Original())(mgr, talk, key, owner, extra, location, rotation, preset,
                                                       presetPlayer, hold, kind, flagA, flagB, preset2, presetPlayer2);
    if (const auto keyName = ReadField<std::uint64_t>(key, 0);
        g_scooterKey != nullptr && keyName != 0 && keyName == ReadField<std::uint64_t>(g_scooterKey, 0))
    {
        try
        {
            float at[3]{};
            if (location != nullptr)
            {
                platform::CopyGuarded(at, location, sizeof(at));
            }
            platform::Log(L"interact: Wheelie ride-on point added by 0x{:X} (owner {}, talk {}, kind {}, presets {} / {}, "
                          L"at {:.0f},{:.0f},{:.0f}), leader {}",
                          registrar, Name(ReadField<std::uint64_t>(owner, 0)), talk, kind,
                          Name(ReadField<std::uint64_t>(preset, 0)), Name(ReadField<std::uint64_t>(presetPlayer, 0)),
                          at[0], at[1], at[2], LeaderText());
        }
        catch (...)
        {
        }
    }
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // the registration lines and the per-registrar counts are Debug
    }
    try
    {
        Lock lock(g_addLock);
        ++g_addsByRegistrar[registrar];
    }
    catch (...)
    {
    }
    try
    {
        float at[3]{};
        if (location != nullptr)
        {
            platform::CopyGuarded(at, location, sizeof(at));
        }
        platform::LogDebug(L"interact: add {} (owner {}, talk {}, kind {}, presets {} / {}{}{}, at {:.0f},{:.0f},{:.0f}) by "
                      L"0x{:X}, leader type {}",
                      Name(ReadField<std::uint64_t>(key, 0)), Name(ReadField<std::uint64_t>(owner, 0)), talk, kind,
                      Name(ReadField<std::uint64_t>(preset, 0)), Name(ReadField<std::uint64_t>(presetPlayer, 0)),
                      preset2 != 0 ? L", second " + Name(preset2) : std::wstring(),
                      presetPlayer2 != 0 ? L" / " + Name(presetPlayer2) : std::wstring(), at[0], at[1], at[2],
                      registrar, ReadField<std::uint8_t>(Party(), kPartyManagerLeaderType));
    }
    catch (...)
    {
    }
}

void ExecuteSlot(void* mgr, std::uint8_t slot) noexcept
{
    const auto* record = static_cast<const std::uint8_t*>(mgr) + kSlotRecords + kSlotRecordSize * (slot & 7);
    const auto  key    = ReadField<std::uint64_t>(record, 0);
    const auto  owner  = ReadField<std::uint64_t>(record, 8);
    const int   outer  = t_marksInExecute;
    t_marksInExecute   = 0;
    reinterpret_cast<ExecuteSlotFn>(g_executeSlot.Original())(mgr, slot);
    const int marks  = t_marksInExecute;
    t_marksInExecute = outer;
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"interact: execute slot {} key {} (owner {}): leader {}, primary {}; executed {}, pending {}, "
                      L"WorldItem marks {}",
                      slot, Name(key), Name(owner), LeaderText(), PrimaryText(),
                      ReadField<std::uint8_t>(mgr, kFieldActionSlotExecuted + (slot & 7)),
                      ReadField<std::uint8_t>(mgr, kFieldActionPending),
                      marks);
    }
    catch (...)
    {
    }
}

/// The pass's mark hook (player_two/field_pickup): a WorldItem key marked.
void OnMarked(std::uint64_t key, std::uint8_t taken, std::uintptr_t returnAddress, bool playerTwo) noexcept
{
    if (t_marksInExecute >= 0 && taken != 0)
    {
        ++t_marksInExecute;
    }
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        const auto caller = Ida(returnAddress);
        platform::LogDebug(L"interact: WorldItem mark {} -> {} ({}, from 0x{:X}){}", Name(key), taken,
                           caller == kMarkFromListener ? L"the key listener" : L"another caller", caller,
                           playerTwo ? L", in player 2's pass" : L"");
    }
    catch (...)
    {
    }
}

/// The pass's grant hook, before the game's grant: each controller's active force-feedback effects (RE-p2-rumble.md §6
/// item 4: expect +1 on the primary one). The grant's line is Debug: its reads are made only when it is written.
void OnGranting(std::uint64_t, bool) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    t_effectsBefore[0] = rumble_watch::EffectCount(generation::PlayerOneController());
    t_effectsBefore[1] = rumble_watch::EffectCount(generation::PlayerTwoController());
}

/// ... and after it, inside the grant's scope: who the rumble went to.
void OnGranted(std::uint64_t key, bool playerTwo) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"interact: WorldItem grant {}{} (the item-get rumble goes to the primary controller: {}; "
                           L"force-feedback effects: player 1's controller {} -> {}, player 2's {} -> {})",
                           Name(key), playerTwo ? L" (player 2's pickup)" : L"", PrimaryText(), t_effectsBefore[0],
                           rumble_watch::EffectCount(generation::PlayerOneController()), t_effectsBefore[1],
                           rumble_watch::EffectCount(generation::PlayerTwoController()));
    }
    catch (...)
    {
    }
}

// --- After each selection update (game thread) -----------------------------------------------------------

/// The slot block, logged when it changes.
void LogSlots(const std::uint8_t* mgr)
{
    if (!platform::Enabled(platform::Level::Trace))
    {
        return;  // the slots flip with each held or pressed button: a per-input report
    }
    std::array<std::uint8_t, sizeof(g_slotSeen)> now{};
    std::size_t                                  at = 0;
    platform::CopyGuarded(now.data() + at, mgr + kFieldActionSlotTargets, 8 * kFieldActionSlots);
    at += 8 * kFieldActionSlots;
    platform::CopyGuarded(now.data() + at, mgr + kFieldActionSlotRows, 8 * kFieldActionSlots);
    at += 8 * kFieldActionSlots;
    platform::CopyGuarded(now.data() + at, mgr + kFieldActionSlotHeld, kFieldActionSlots);
    platform::CopyGuarded(now.data() + at + kFieldActionSlots, mgr + kFieldActionSlotPressed, kFieldActionSlots);
    platform::CopyGuarded(now.data() + at + 2 * kFieldActionSlots, mgr + kFieldActionSlotExecuted, kFieldActionSlots);
    at += 3 * kFieldActionSlots;
    now[at] = ReadField<std::uint8_t>(mgr, kFieldActionPending);
    if (now == g_slotSeen)
    {
        return;
    }
    g_slotSeen = now;
    std::wstring slots;
    for (int i = 0; i < kFieldActionSlots; ++i)
    {
        const auto target = ReadField<std::uint64_t>(mgr, kFieldActionSlotTargets + 8 * i);
        const auto row    = ReadField<std::uint64_t>(mgr, kFieldActionSlotRows + 8 * i);
        const auto held   = ReadField<std::uint8_t>(mgr, kFieldActionSlotHeld + i);
        const auto press  = ReadField<std::uint8_t>(mgr, kFieldActionSlotPressed + i);
        const auto done   = ReadField<std::uint8_t>(mgr, kFieldActionSlotExecuted + i);
        if (target == 0 && row == 0 && held == 0 && press == 0 && done == 0)
        {
            continue;
        }
        slots += std::format(L"{}[{}] {}{}{}{}{}", slots.empty() ? L"" : L"; ", i, Name(target),
                             row != 0 ? L", row" : L"", held != 0 ? L", held" : L"", press != 0 ? L", pressed" : L"",
                             done != 0 ? L", executed" : L"");
    }
    platform::LogTrace(L"interact: slots {} (candidates {}, pending {}, leader {})", slots.empty() ? L"none" : slots,
                  ReadField<std::int32_t>(mgr, kCandidateCount), now[at], LeaderText());
}

/// The keys in WorldItemMgr's map (allocated elements only; platform/sparse_array.h).
std::unordered_set<std::uint64_t> WorldItemKeys(const std::uint8_t* items)
{
    std::unordered_set<std::uint64_t> keys;
    if (items == nullptr)
    {
        return keys;
    }
    platform::ForEachSparseElement(
        reinterpret_cast<std::uintptr_t>(items + kWorldItemMapElements), kWorldItemMapStride,
        [&keys](std::uintptr_t element) {
            keys.insert(ReadField<std::uint64_t>(reinterpret_cast<const void*>(element), 0));
            return true;
        },
        kMaxItems);
    return keys;
}

/// The points within kNearCm of either player, each logged when it first comes near, when its flags,
/// slot or nearness change, and when it leaves.
void Census(const std::uint8_t* mgr, const std::unordered_set<std::uint64_t>& items, const float (&one)[3],
            const float (&two)[3])
{
    struct Near
    {
        std::uint64_t key;
        PointSeen     seen;
        const std::uint8_t* point;
        float         d1, d2;
    };
    std::vector<Near> nearby;
    nearby.reserve(kFieldActionMaxPoints);  // nothing allocates while the game's lock is held
    int           walked = 0;
    float         nearestOne = 1e30f, nearestTwo = 1e30f;
    std::uint64_t nearestOneKey = 0, nearestTwoKey = 0;
    {
        const SharedLock lock(reinterpret_cast<PSRWLOCK>(const_cast<std::uint8_t*>(mgr) + kFieldActionPointLock));
        const auto* node = PointerAt(mgr, kFieldActionPointList);
        for (int n = 0; node != nullptr && n < kFieldActionMaxPoints; ++n, node = PointerAt(node, kFieldActionNodeNext))
        {
            const auto* point = node + kFieldActionNodePoint;
            float       at[3]{};
            platform::CopyGuarded(at, point + kPointLocation, sizeof(at));
            const float d1 = Distance(at, one), d2 = Distance(at, two);
            ++walked;
            if (d1 < nearestOne)
            {
                nearestOne    = d1;
                nearestOneKey = ReadField<std::uint64_t>(point, kFieldActionPointKey);
            }
            if (d2 < nearestTwo)
            {
                nearestTwo    = d2;
                nearestTwoKey = ReadField<std::uint64_t>(point, kFieldActionPointKey);
            }
            if (d1 > kNearCm && d2 > kNearCm)
            {
                continue;
            }
            PointSeen seen;
            seen.flags   = ReadField<std::uint32_t>(point, kFieldActionPointFlags);
            seen.slot    = ReadField<std::uint8_t>(point, kPointSlot);
            seen.talk    = ReadField<std::uint8_t>(point, kPointTalk);
            seen.nearOne = d1 <= kNearCm;
            seen.nearTwo = d2 <= kNearCm;
            nearby.push_back({ReadField<std::uint64_t>(point, kFieldActionPointKey), seen, point, d1, d2});
        }
    }
    // A summary every kRegistrarPeriodMs, so an empty census reads as "nothing near", not as a broken walk.
    if (const ULONGLONG now = GetTickCount64(); now >= g_censusSummaryAt)
    {
        g_censusSummaryAt = now + kRegistrarPeriodMs;
        platform::LogDebug(L"interact: census: {} point(s) registered, {} within 20 m of a player; nearest to player 1 {} at "
                      L"{:.1f} m, to player 2 {} at {:.1f} m",
                      walked, nearby.size(), Name(nearestOneKey), nearestOne / 100.0f, Name(nearestTwoKey),
                      nearestTwo / 100.0f);
    }
    std::unordered_set<std::uint64_t> present;
    for (const Near& n : nearby)
    {
        present.insert(n.key);
        const auto it    = g_points.find(n.key);
        const bool first = it == g_points.end();
        if (!first && it->second == n.seen)
        {
            continue;
        }
        g_points[n.key] = n.seen;
        if (first)
        {
            platform::LogDebug(L"interact: near {} (owner {}, {}, talk {}, presets {} / {} / {}, slot {}, kind {}, variant {}, "
                          L"flags 0x{:08X}): {:.1f} m from player 1, {:.1f} m from player 2",
                          Name(n.key), Name(ReadField<std::uint64_t>(n.point, kPointOwner)),
                          items.contains(n.key) ? L"WorldItem" : L"not a WorldItem", n.seen.talk,
                          Name(ReadField<std::uint64_t>(n.point, kFieldActionPointPreset)),
                          Name(ReadField<std::uint64_t>(n.point, kPointPreset2)),
                          Name(ReadField<std::uint64_t>(n.point, kPointPresetPlayer)), n.seen.slot,
                          ReadField<std::uint8_t>(n.point, kPointKind), ReadField<std::uint8_t>(n.point, kPointVariant),
                          n.seen.flags, n.d1 / 100.0f, n.d2 / 100.0f);
        }
        else
        {
            platform::LogDebug(L"interact: {} now slot {}, flags 0x{:08X}{}: {:.1f} m from player 1, {:.1f} m from player 2",
                          Name(n.key), n.seen.slot, n.seen.flags,
                          (n.seen.flags & 2) != 0 ? L" (player 1's slot target)" : L"", n.d1 / 100.0f, n.d2 / 100.0f);
        }
    }
    for (auto it = g_points.begin(); it != g_points.end();)
    {
        if (present.contains(it->first))
        {
            ++it;
            continue;
        }
        platform::LogDebug(L"interact: {} is no longer near either player (or was removed)", Name(it->first));
        it = g_points.erase(it);
    }
}

/// The game's candidate query around player 2's member, into the mod's own array (writes nothing else).
void DryRun(const std::uint8_t* mgr, const std::unordered_set<std::uint64_t>& items, const float (&two)[3])
{
    if (g_query == nullptr)
    {
        return;
    }
    alignas(16) float box[8] = {two[0], two[1], two[2], 0.0f, kQueryExtent, kQueryExtent, kQueryExtent, 0.0f};
    RawArray          found{g_found.data(), 0, kFieldActionMaxPoints};  // the pool's size: the query never reallocates
    void*             cap     = &found;
    auto*             octree  = const_cast<std::uint8_t*>(mgr) + kOctree;
    if (!platform::CallGuarded(g_query, static_cast<void*>(octree), std::int32_t{0}, static_cast<void*>(octree),
                               static_cast<const float*>(box), static_cast<void*>(&cap)))
    {
        g_query = nullptr;
        platform::Log(L"interact: the dry-run query faulted; it is not run again");
        return;
    }
    std::vector<std::uint64_t> keys;
    const int                  count = std::clamp(found.num, 0, kFieldActionMaxPoints);
    keys.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        keys.push_back(ReadField<std::uint64_t>(g_found[i], kFieldActionPointKey));
    }
    std::sort(keys.begin(), keys.end());
    if (keys == g_dryRunSeen)
    {
        return;
    }
    g_dryRunSeen = keys;
    std::wstring line;
    for (int i = 0; i < count; ++i)
    {
        const auto* point = static_cast<const std::uint8_t*>(g_found[i]);
        const auto  key   = ReadField<std::uint64_t>(point, kFieldActionPointKey);
        float       at[3]{};
        platform::CopyGuarded(at, point + kPointLocation, sizeof(at));
        line += std::format(L"{}{} ({}, {:.1f} m)", line.empty() ? L"" : L"; ", Name(key),
                            items.contains(key) ? L"WorldItem"
                                                : Name(ReadField<std::uint64_t>(point, kFieldActionPointPreset)),
                            Distance(at, two) / 100.0f);
    }
    platform::LogDebug(L"interact: dry run around player 2's member: {} candidate(s){}{}", count, count != 0 ? L": " : L"",
                  line);
}

/// The registrations since the last summary, per registrar (the return address into the caller).
void LogRegistrars()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_registrarsAt)
    {
        return;
    }
    g_registrarsAt = now + kRegistrarPeriodMs;
    std::unordered_map<std::uintptr_t, std::uint64_t> counts;
    {
        Lock lock(g_addLock);
        counts.swap(g_addsByRegistrar);
    }
    if (counts.empty())
    {
        return;
    }
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end());
    std::wstring line;
    for (const auto& [registrar, count] : sorted)
    {
        line += std::format(L"{}0x{:X} {}", line.empty() ? L"" : L", ", registrar, count);
    }
    platform::LogDebug(L"interact: registrations in the last period by registrar (return address): {}", line);
}

void AfterUpdate(const std::uint8_t* mgr)
{
    LogSlots(mgr);
    const bool debug = platform::Enabled(platform::Level::Debug);  // the registrars, census and dry run are Debug
    if (debug)
    {
        LogRegistrars();
    }
    const bool in = generation::Active() && generation::Member() != nullptr;
    if (in != g_wasIn)
    {
        g_wasIn = in;
        g_points.clear();
        g_dryRunSeen.clear();
    }
    const ULONGLONG now = GetTickCount64();
    if (!debug || !in || now < g_censusAt)
    {
        return;
    }
    g_censusAt = now + kCensusPeriodMs;
    float one[3]{}, two[3]{};
    void* leader = Leader();
    if (leader == nullptr || !players::unreal::ActorLocation(leader, one) ||
        !players::unreal::ActorLocation(generation::Member(), two))
    {
        return;
    }
    const auto items = WorldItemKeys(PointerAt(Root(), kRootWorldItems));
    Census(mgr, items, one, two);
    DryRun(mgr, items, two);
}

/// The pass's update hook (game thread): after the game's own update and player 2's pass.
void OnUpdate(void* mgr) noexcept
{
    try
    {
        AfterUpdate(static_cast<const std::uint8_t*>(mgr));
    }
    catch (...)
    {
    }
}

void Summary(std::wstring& installed, std::wstring& skipped, const wchar_t* what, bool ok)
{
    (ok ? installed : skipped) += std::format(L"{}{}", (ok ? installed : skipped).empty() ? L"" : L", ", what);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image          = image;
    const auto root  = image.Rebase(kEndGameRoot, sizeof(void*));
    const auto party = image.Rebase(kPartyManagerGlobal, sizeof(void*));
    if (!root || !party || !leader_scope::Installed())
    {
        platform::Log(L"interact: the root or party manager is outside the image; not watched");
        return;
    }
    g_root  = *root;
    g_party = *party;
    if (const auto scooterKey = image.Rebase(kScooterKey, sizeof(std::uint64_t)))
    {
        g_scooterKey = reinterpret_cast<const std::uint64_t*>(*scooterKey);
    }

    const auto entry = [&](platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
                           std::uintptr_t replacement) {
        const auto at    = platform::FindFunction(image, fact);
        const auto first = fact.entryBytes.first(kFirstInstruction);
        return at && redirect.Install(*at, image.EntryInstruction(fact.address, first), replacement);
    };
    bool add = false;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        const auto bytes = kAddPoint.entryBytes;
        const auto at    = platform::FindFunction(image, kAddPoint);
        add = at &&
              g_addPoint.InstallAtSecondInstruction(*at, bytes.first(kFirstInstruction),
                                                    bytes.subspan(kFirstInstruction, kSecondInstruction),
                                                    reinterpret_cast<std::uintptr_t>(&AddPoint));
    }
    else
    {
        add = entry(g_addPoint, kAddPoint, reinterpret_cast<std::uintptr_t>(&AddPoint));
    }
    const bool execute = entry(g_executeSlot, kExecuteSlot, reinterpret_cast<std::uintptr_t>(&ExecuteSlot));
    if (const auto query = platform::FindFunction(image, kQuery))
    {
        g_query = reinterpret_cast<QueryFn>(*query);
    }
    // The update, the marks and the grants: told by the pass's own hooks (installed or not, as its line says).
    field_pickup::SetHookObserver({&OnUpdate, &OnMarked, &OnGranting, &OnGranted});
    try
    {
        std::wstring installed, skipped;
        Summary(installed, skipped, L"registrations", add);
        Summary(installed, skipped, L"executions", execute);
        Summary(installed, skipped, L"the dry-run query", g_query != nullptr);
        platform::Log(L"interact: watching {}, and the pass's update, marks and grants through its hooks; not watched (the "
                      L"code is not what the research read): {}",
                      installed.empty() ? L"nothing" : installed, skipped.empty() ? L"none" : skipped);
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    field_pickup::SetHookObserver({});  // the pass tells no one
    bool restored = g_executeSlot.Restore();
    restored      = g_addPoint.Restore() && restored;
    if (!restored)
    {
        platform::Log(L"interact: an entry was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::player_two::interact_watch
