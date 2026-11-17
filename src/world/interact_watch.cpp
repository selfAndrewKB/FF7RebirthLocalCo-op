#include "world/interact_watch.h"

#include "facts/field_action.h"
#include "facts/party.h"
#include "facts/world.h"
#include "feedback/rumble_watch.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "world/field_pickup.h"

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

namespace lss::world::interact_watch {

namespace {

// The objects (research/RE-p2-field-interact.md; this build).
using facts::kRootWorldItems;
using facts::kPartyManagerPrimaryController;
using facts::kPartyManagerLeader;
using facts::kPartyManagerLeaderType;

// FieldActionMgr.
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

// A point.
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

// WorldItemMgr's map of items: a stock TMap keyed by the item's FName at element +0 (facts/world.h).
using facts::kWorldItemMapElements;
using facts::kWorldItemMapStride;
constexpr std::int32_t  kMaxItems       = 16384;
using facts::kFieldActionEntryKeyBack;

// The hooked functions. Entry redirects replace the first instruction (5 bytes); the rest of the
// listed bytes are checked too.
constexpr std::uint8_t   kAddPointBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C,
                                             0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
// void(mgr, talk, key*, owner*, u64*, loc*, rot*, preset*, presetPlayer*, float, kind, u8, u8, FName, FName)
constexpr platform::FunctionFact kAddPoint{L"FieldActionMgr's AddPoint", 0x140FED95C, kAddPointBytes};
constexpr std::uint8_t   kExecuteSlotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// void(mgr, u8 slot)
constexpr platform::FunctionFact kExecuteSlot{L"FieldActionMgr's ExecuteSlot", 0x14117D390, kExecuteSlotBytes};
constexpr std::size_t    kFirstInstruction = 5;
// 1.005 (research/RE-1005-field-2.md): AddPoint's entry sits at %16 = 12, so its first instruction crosses a
// 16-byte block. Its second (mov [rsp+20h], r9 at %16 = 1) takes the redirect instead: the first only stores rbx into
// the caller's home area and nothing branches to the second, so the replacement is entered as at the entry, with every
// caller's registration, on any thread (the trampoline replays both instructions).
constexpr std::size_t    kSecondInstruction = 5;
// The Wheelie ride-on point's key (an FName global, research/RE-p2-wheelie-ride.md): no analysed code adds it, so its
// registration is logged at Info to name the registrar.
constexpr std::uintptr_t kScooterKey = 0x14901EDC0;
constexpr std::uintptr_t kMarkFromListener = 0x14117CFFD;  // the WorldItem key listener's call returns here
// The candidate query, called read-only for the dry run: void(octree, node, context, box*, cap*).
constexpr std::uint8_t   kQueryBytes[] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr platform::FunctionFact kQuery{L"the octree's candidate query", 0x1409A41E4, kQueryBytes};
constexpr float          kQueryExtent  = 10.0f;  // as the update's call (cm)

constexpr float     kNearCm         = 2000.0f;  // the census: points within 20 m of any player
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
thread_local std::int32_t                   t_effectsBefore[players::kMaxPlayers]{};  // by player: effects before a grant

// Game thread (the update hook).
std::array<std::uint8_t, 8 * kFieldActionSlots + kFieldActionSlots * 8 + 3 * kFieldActionSlots + 1> g_slotSeen{};
ULONGLONG                                g_censusAt = 0;
ULONGLONG                                g_registrarsAt = 0;
ULONGLONG                                g_censusSummaryAt = 0;
struct PointSeen
{
    std::uint32_t flags = 0;
    std::uint8_t  slot = 0, talk = 0;
    bool          nearTo[players::kMaxPlayers]{};  // by player (0 player 1): within kNearCm of their character
    bool operator==(const PointSeen&) const = default;
};
std::unordered_map<std::uint64_t, PointSeen> g_points;  // key -> last logged
std::vector<std::uint64_t>                   g_dryRunSeen[players::kMaxPlayers];  // by seat: the keys last logged
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
    return g_image ? g_image->SourceAddress(live) : live;
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
    return party != nullptr ? scopes::leader_scope::ObjectAt(party + kPartyManagerLeader) : nullptr;
}

void* PrimaryController() noexcept
{
    const auto* party = Party();
    return party != nullptr ? scopes::leader_scope::ObjectAt(party + kPartyManagerPrimaryController) : nullptr;
}

/// The leader and primary controller for the log: resolved only on the game thread (the weak-pointer resolve is
/// game-thread code).
std::wstring LeaderText()
{
    return players::generation::OnGameThread() ? players::Who(Leader()) : std::wstring(L"(off the game thread)");
}

std::wstring PrimaryText()
{
    return players::generation::OnGameThread() ? players::Who(PrimaryController()) : std::wstring(L"(off the game thread)");
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

/// The pass's mark hook (world/field_pickup): a WorldItem key marked.
void OnMarked(std::uint64_t key, std::uint8_t taken, std::uintptr_t returnAddress, int seat) noexcept
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
                           seat != 0 ? std::format(L", in {}'s pass", players::SeatName(seat)) : std::wstring());
    }
    catch (...)
    {
    }
}

/// The pass's grant hook, before the game's grant: each controller's active force-feedback effects (research/RE-p2-rumble.md
/// item 4: expect +1 on the primary one). The grant's line is Debug: its reads are made only when it is written.
void OnGranting(std::uint64_t, int) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    for (int p = 0; p < players::kMaxPlayers; ++p)
    {
        t_effectsBefore[p] = feedback::rumble_watch::EffectCount(players::generation::ControllerOf(p));
    }
}

/// ... and after it, inside the grant's scope: who the rumble went to.
void OnGranted(std::uint64_t key, int seat) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        std::wstring effects;  // every controller there is: its effects before and after
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            if (const void* controller = players::generation::ControllerOf(p); controller != nullptr)
            {
                effects += std::format(L"{}{}'s controller {} -> {}", effects.empty() ? L"" : L", ", players::SeatName(p),
                                       t_effectsBefore[p], feedback::rumble_watch::EffectCount(controller));
            }
        }
        platform::LogDebug(L"interact: WorldItem grant {}{} (the item-get rumble goes to the primary controller: {}; "
                           L"force-feedback effects: {})",
                           Name(key), seat != 0 ? std::format(L" ({}'s pickup)", players::SeatName(seat)) : std::wstring(),
                           PrimaryText(), effects);
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

/// Where every player's character stands, by seat (index 0 player 1's); `in` is false for a seat with no member.
struct Characters
{
    bool  in[players::kMaxPlayers]{};
    float at[players::kMaxPlayers][3]{};
};

/// "3.0 m from player 1, 12.5 m from player 2": `d` (cm, by player) for every player in.
std::wstring FromPlayers(const Characters& who, const float (&d)[players::kMaxPlayers])
{
    std::wstring text;
    for (int p = 0; p < players::kMaxPlayers; ++p)
    {
        if (who.in[p])
        {
            text += std::format(L"{}{:.1f} m from {}", text.empty() ? L"" : L", ", d[p] / 100.0f, players::SeatName(p));
        }
    }
    return text;
}

/// The points within kNearCm of any player, each logged when it first comes near, when its flags,
/// slot or nearness change, and when it leaves.
void Census(const std::uint8_t* mgr, const std::unordered_set<std::uint64_t>& items, const Characters& who)
{
    struct Near
    {
        std::uint64_t key;
        PointSeen     seen;
        const std::uint8_t* point;
        float         d[players::kMaxPlayers];
    };
    std::vector<Near> nearby;
    nearby.reserve(kFieldActionMaxPoints);  // nothing allocates while the game's lock is held
    int           walked = 0;
    float         nearest[players::kMaxPlayers];
    std::uint64_t nearestKey[players::kMaxPlayers]{};
    std::fill(std::begin(nearest), std::end(nearest), 1e30f);
    {
        const SharedLock lock(reinterpret_cast<PSRWLOCK>(const_cast<std::uint8_t*>(mgr) + kFieldActionPointLock));
        const auto* node = PointerAt(mgr, kFieldActionPointList);
        for (int n = 0; node != nullptr && n < kFieldActionMaxPoints; ++n, node = PointerAt(node, kFieldActionNodeNext))
        {
            const auto* point = node + kFieldActionNodePoint;
            float       at[3]{};
            platform::CopyGuarded(at, point + kPointLocation, sizeof(at));
            ++walked;
            Near found{ReadField<std::uint64_t>(point, kFieldActionPointKey), PointSeen{}, point, {}};
            bool anyNear = false;
            for (int p = 0; p < players::kMaxPlayers; ++p)
            {
                if (!who.in[p])
                {
                    continue;
                }
                found.d[p] = Distance(at, who.at[p]);
                if (found.d[p] < nearest[p])
                {
                    nearest[p]    = found.d[p];
                    nearestKey[p] = found.key;
                }
                found.seen.nearTo[p] = found.d[p] <= kNearCm;
                anyNear             = anyNear || found.seen.nearTo[p];
            }
            if (!anyNear)
            {
                continue;
            }
            found.seen.flags = ReadField<std::uint32_t>(point, kFieldActionPointFlags);
            found.seen.slot  = ReadField<std::uint8_t>(point, kPointSlot);
            found.seen.talk  = ReadField<std::uint8_t>(point, kPointTalk);
            nearby.push_back(found);
        }
    }
    // A summary every kRegistrarPeriodMs, so an empty census reads as "nothing near", not as a broken walk.
    if (const ULONGLONG now = GetTickCount64(); now >= g_censusSummaryAt)
    {
        g_censusSummaryAt = now + kRegistrarPeriodMs;
        std::wstring nearestText;
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            if (who.in[p])
            {
                nearestText += std::format(L"{}to {} {} at {:.1f} m", nearestText.empty() ? L"" : L", ", players::SeatName(p),
                                           Name(nearestKey[p]), nearest[p] / 100.0f);
            }
        }
        platform::LogDebug(L"interact: census: {} point(s) registered, {} within 20 m of a player; nearest {}", walked,
                           nearby.size(), nearestText);
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
                          L"flags 0x{:08X}): {}",
                          Name(n.key), Name(ReadField<std::uint64_t>(n.point, kPointOwner)),
                          items.contains(n.key) ? L"WorldItem" : L"not a WorldItem", n.seen.talk,
                          Name(ReadField<std::uint64_t>(n.point, kFieldActionPointPreset)),
                          Name(ReadField<std::uint64_t>(n.point, kPointPreset2)),
                          Name(ReadField<std::uint64_t>(n.point, kPointPresetPlayer)), n.seen.slot,
                          ReadField<std::uint8_t>(n.point, kPointKind), ReadField<std::uint8_t>(n.point, kPointVariant),
                          n.seen.flags, FromPlayers(who, n.d));
        }
        else
        {
            platform::LogDebug(L"interact: {} now slot {}, flags 0x{:08X}{}: {}", Name(n.key), n.seen.slot, n.seen.flags,
                               (n.seen.flags & 2) != 0 ? L" (player 1's slot target)" : L"", FromPlayers(who, n.d));
        }
    }
    for (auto it = g_points.begin(); it != g_points.end();)
    {
        if (present.contains(it->first))
        {
            ++it;
            continue;
        }
        platform::LogDebug(L"interact: {} is no longer near any player (or was removed)", Name(it->first));
        it = g_points.erase(it);
    }
}

/// The game's candidate query around `seat`'s member (at `member`), into the mod's own array (writes nothing else).
void DryRun(const std::uint8_t* mgr, const std::unordered_set<std::uint64_t>& items, int seat, const float (&member)[3])
{
    if (g_query == nullptr)
    {
        return;
    }
    alignas(16) float box[8] = {member[0], member[1], member[2], 0.0f, kQueryExtent, kQueryExtent, kQueryExtent, 0.0f};
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
    if (keys == g_dryRunSeen[seat])
    {
        return;
    }
    g_dryRunSeen[seat] = keys;
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
                            Distance(at, member) / 100.0f);
    }
    platform::LogDebug(L"interact: dry run around {}'s member: {} candidate(s){}{}", players::SeatName(seat), count,
                       count != 0 ? L": " : L"", line);
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
    bool in = false;  // a seat's player holds a member
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        in = in || (players::generation::Active(seat) && players::generation::MemberOf(seat) != nullptr);
    }
    if (in != g_wasIn)
    {
        g_wasIn = in;
        g_points.clear();
        for (auto& seen : g_dryRunSeen)
        {
            seen.clear();
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (!debug || !in || now < g_censusAt)
    {
        return;
    }
    g_censusAt = now + kCensusPeriodMs;
    Characters who;
    void*      leader = Leader();
    who.in[0]         = leader != nullptr && players::unreal::ActorLocation(leader, who.at[0]);
    bool anySeat      = false;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        void* member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        who.in[seat] = member != nullptr && players::unreal::ActorLocation(member, who.at[seat]);
        anySeat      = anySeat || who.in[seat];
    }
    if (!who.in[0] || !anySeat)
    {
        return;
    }
    const auto items = WorldItemKeys(PointerAt(Root(), kRootWorldItems));
    Census(mgr, items, who);
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        if (who.in[seat])
        {
            DryRun(mgr, items, seat, who.at[seat]);
        }
        else
        {
            g_dryRunSeen[seat].clear();  // a seat's player who comes back starts with a line
        }
    }
}

/// The pass's update hook (game thread): after the game's own update and the seats' passes.
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
    const auto root  = players::game_globals::Address(players::game_globals::Global::GameRoot);
    const auto party = players::game_globals::Address(players::game_globals::Global::PartyManager);
    if (!root || !party || !scopes::leader_scope::Installed())
    {
        platform::Log(L"interact: the root or party manager is outside the image; not watched");
        return;
    }
    g_root  = root;
    g_party = party;
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
    if (platform::RunningBuild() != platform::BuildIndex::V1004)
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

}  // namespace lss::world::interact_watch
