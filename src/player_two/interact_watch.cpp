#include "player_two/interact_watch.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
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
constexpr std::uintptr_t kGameRoot          = 0x1490FEDE8;  // pointer global: the engine root
constexpr std::uint32_t  kRootActionManager = 0x1A0;        // FieldActionMgr
constexpr std::uint32_t  kRootWorldItems    = 0x288;        // WorldItemMgr
constexpr std::uintptr_t kPartyManager      = 0x14939BCE0;  // pointer global
constexpr std::uint32_t  kPartyController   = 0x140;        // FWeakObjectPtr: the primary controller
constexpr std::uint32_t  kPartyLeader       = 0x148;        // FWeakObjectPtr: the leader
constexpr std::uint32_t  kPartyLeaderType   = 0x150;        // u8

// FieldActionMgr (§1.2, §1.3, §10.D).
constexpr std::uint32_t kOctree         = 0x10;
constexpr std::uint32_t kCandidateCount = 0x298;    // i32
constexpr std::uint32_t kPointList      = 0x48320;  // the live list's head node; node+0x08 next, point = node+0x18
constexpr std::uint32_t kNodeNext       = 0x08;
constexpr std::uint32_t kNodePoint      = 0x18;
constexpr std::uint32_t kPointLock      = 0x487F0;  // SRWLOCK
constexpr std::uint32_t kSlotRows       = 0x483A8;  // 8 x FWeakObjectPtr
constexpr std::uint32_t kSlotRecords    = 0x484A8;  // 8 x 36 bytes: +0 key, +8 owner, +0x18 hold timer
constexpr std::uint32_t kSlotRecordSize = 36;
constexpr std::uint32_t kSlotTargets    = 0x48608;  // 8 x FName: this frame's target keys
constexpr std::uint32_t kSlotHeld       = 0x48730;  // 8 x u8
constexpr std::uint32_t kSlotPressed    = 0x48738;  // 8 x u8
constexpr std::uint32_t kSlotExecuted   = 0x48740;  // 8 x u8
constexpr std::uint32_t kPending        = 0x487D0;  // u8
constexpr int           kSlots          = 8;
constexpr int           kMaxPoints      = 1024;     // the pool's size

// A point (§1.2 as corrected in §10.B).
constexpr std::uint32_t kPointTalk          = 0x08;   // u8
constexpr std::uint32_t kPointKey           = 0x0C;   // FName
constexpr std::uint32_t kPointOwner         = 0x1C;   // FName
constexpr std::uint32_t kPointLocation      = 0x2C;   // FVector
constexpr std::uint32_t kPointPreset        = 0x44;   // FName rpa<Owner>
constexpr std::uint32_t kPointPreset2       = 0x4C;   // FName, a second point-side preset
constexpr std::uint32_t kPointPresetPlayer  = 0x54;   // FName rpa<Owner>Player
constexpr std::uint32_t kPointSlot          = 0xE8;   // u8
constexpr std::uint32_t kPointKind          = 0xF0;   // u8
constexpr std::uint32_t kPointVariant       = 0xF1;   // u8
constexpr std::uint32_t kPointFlags         = 0x104;  // 4 bytes: bit 1 slot target, 2 runner-up, 4 skipped by the query

// WorldItemMgr's map of items (§10.E): a stock TMap, elements of 0xD0 bytes with the key FName at +0.
constexpr std::uint32_t kItemElements   = 0x90;
constexpr std::uint32_t kItemCount      = 0x98;  // i32
constexpr std::uint32_t kItemBitsInline = 0xA0;
constexpr std::uint32_t kItemBitsHeap   = 0xB0;
constexpr std::uint32_t kItemStride     = 0xD0;
constexpr std::int32_t  kMaxItems       = 16384;
constexpr std::uint32_t kEntryKeyBack   = 0x10;  // AcquireEntry's entry = element + 0x10

// The hooked functions (§10.A). Entry redirects replace the first instruction (5 bytes); the rest of the
// listed bytes are checked too.
constexpr std::uintptr_t kAddPoint        = 0x1411BDE60;  // void(mgr, talk, key*, owner*, u64*, loc*, rot*, preset*, presetPlayer*, float, kind, u8, u8, FName, FName)
constexpr std::uint8_t   kAddPointBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C,
                                             0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
constexpr std::uintptr_t kExecuteSlot        = 0x1414A90E4;  // void(mgr, u8 slot)
constexpr std::uint8_t   kExecuteSlotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uintptr_t kMarkByKey        = 0x140C10F94;  // u8(wim, FName key by value)
constexpr std::uint8_t   kMarkByKeyBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr std::size_t    kFirstInstruction = 5;
constexpr std::uintptr_t kMarkFromListener = 0x1414A90AD;  // the WorldItem key listener's call returns here
// Call redirects: the game mode tick's call of the selection update, and the WorldItem tick's call of the grant.
constexpr std::uintptr_t kUpdateCall  = 0x1423EF160;
constexpr std::uintptr_t kUpdate      = 0x140E7A700;  // void(mgr, float dt)
constexpr std::uintptr_t kAcquireCall = 0x140DBE165;
constexpr std::uintptr_t kAcquire     = 0x140C07A3C;  // (wim, entry); the caller ignores its result
// The candidate query, called read-only for the dry run (§10.C): void(octree, node, context, box*, cap*).
constexpr std::uintptr_t kQuery        = 0x1422BA490;
constexpr std::uint8_t   kQueryBytes[] = {0x41, 0x55, 0x41, 0x56, 0xB8, 0x28, 0x01, 0x00, 0x00};
constexpr float          kQueryExtent  = 10.0f;  // as the update's call (cm)

constexpr float     kNearCm         = 2000.0f;  // the census: points within 20 m of either player
constexpr ULONGLONG kCensusPeriodMs = 500;
constexpr ULONGLONG kBudgetPeriodMs = 10'000;
constexpr ULONGLONG kRegistrarPeriodMs = 30'000;
constexpr int       kBudgetLines    = 300;  // lines per period; the rest are counted
constexpr int       kAddLinesInFull = 400;  // registrations logged in full, then counted per registrar

using AddPointFn = void (*)(void* mgr, std::uint8_t talk, const std::uint64_t* key, const std::uint64_t* owner,
                            const std::uint64_t* extra, const float* location, const float* rotation,
                            const std::uint64_t* preset, const std::uint64_t* presetPlayer, float hold,
                            std::uint8_t kind, std::uint8_t flagA, std::uint8_t flagB, std::uint64_t preset2,
                            std::uint64_t presetPlayer2);
using ExecuteSlotFn = void (*)(void* mgr, std::uint8_t slot);
using MarkByKeyFn   = std::uint8_t (*)(void* items, std::uint64_t key);
using UpdateFn      = void (*)(void* mgr, float dt);
using AcquireFn     = void (*)(void* items, void* entry);
using QueryFn       = void (*)(void* octree, std::int32_t node, void* context, const float* box, void* cap);

struct RawArray  // TArray<Point*> with the default allocator
{
    void**       data;
    std::int32_t num;
    std::int32_t max;
};

platform::EntryRedirect g_addPoint;
platform::EntryRedirect g_executeSlot;
platform::EntryRedirect g_markByKey;
platform::CallRedirect  g_update;
platform::CallRedirect  g_acquire;
QueryFn                 g_query     = nullptr;
std::uintptr_t          g_base      = 0;  // the image's live base, for IDA addresses in the log
std::uintptr_t          g_root      = 0;  // live address of the root global
std::uintptr_t          g_party     = 0;  // live address of the party manager global

// The line budget, shared by every thread that logs here.
SRWLOCK          g_budgetLock = SRWLOCK_INIT;
ULONGLONG        g_budgetAt   = 0;
int              g_budgetUsed = 0;
int              g_dropped    = 0;

// Registrations (any thread): logged in full up to kAddLinesInFull, then counted per registrar.
SRWLOCK                                     g_addLock = SRWLOCK_INIT;
int                                         g_addsLogged = 0;
std::unordered_map<std::uintptr_t, std::uint64_t> g_addsByRegistrar;  // IDA return address -> count since summary
thread_local int                            t_marksInExecute = -1;  // -1 outside ExecuteSlot

// Game thread (the update hook).
std::array<std::uint8_t, 8 * kSlots + kSlots * 8 + 3 * kSlots + 1> g_slotSeen{};
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
std::array<void*, kMaxPoints>                g_found{};
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

std::uintptr_t Ida(std::uintptr_t live) noexcept
{
    return live - g_base + 0x140000000;
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"-") : players::unreal::NameText(name);
}

/// Spends one line of the period's budget; false when it is used up (the line is counted instead).
bool Spend(int lines = 1) noexcept
{
    int  droppedLastPeriod = 0;
    bool spent             = false;
    {
        Lock            lock(g_budgetLock);
        const ULONGLONG now = GetTickCount64();
        if (now >= g_budgetAt)
        {
            droppedLastPeriod = g_dropped;
            g_budgetAt        = now + kBudgetPeriodMs;
            g_budgetUsed      = 0;
            g_dropped         = 0;
        }
        spent = g_budgetUsed + lines <= kBudgetLines;
        (spent ? g_budgetUsed : g_dropped) += lines;
    }
    if (droppedLastPeriod != 0)
    {
        platform::Log(L"interact: {} line(s) over the budget were not written in the last period", droppedLastPeriod);
    }
    return spent;
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
    return party != nullptr ? leader_scope::ObjectAt(party + kPartyLeader) : nullptr;
}

void* PrimaryController() noexcept
{
    const auto* party = Party();
    return party != nullptr ? leader_scope::ObjectAt(party + kPartyController) : nullptr;
}

std::wstring Who(void* object)
{
    if (object == nullptr)
    {
        return L"none";
    }
    if (object == generation::PlayerOneController())
    {
        return L"player 1's controller";
    }
    if (object == generation::PlayerTwoController())
    {
        return L"player 2's controller";
    }
    if (object == generation::Member())
    {
        return L"player 2's member";
    }
    return std::format(L"0x{:X}", reinterpret_cast<std::uintptr_t>(object));
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
    bool inFull = false;
    try
    {
        Lock lock(g_addLock);
        ++g_addsByRegistrar[registrar];
        inFull = g_addsLogged < kAddLinesInFull;
        g_addsLogged += inFull ? 1 : 0;
    }
    catch (...)
    {
    }
    if (!inFull || !Spend())
    {
        return;
    }
    try
    {
        float at[3]{};
        if (location != nullptr)
        {
            platform::CopyGuarded(at, location, sizeof(at));
        }
        platform::Log(L"interact: add {} (owner {}, talk {}, kind {}, presets {} / {}{}{}, at {:.0f},{:.0f},{:.0f}) by "
                      L"0x{:X}, leader type {}",
                      Name(Read<std::uint64_t>(key, 0)), Name(Read<std::uint64_t>(owner, 0)), talk, kind,
                      Name(Read<std::uint64_t>(preset, 0)), Name(Read<std::uint64_t>(presetPlayer, 0)),
                      preset2 != 0 ? L", second " + Name(preset2) : std::wstring(),
                      presetPlayer2 != 0 ? L" / " + Name(presetPlayer2) : std::wstring(), at[0], at[1], at[2],
                      registrar, Read<std::uint8_t>(Party(), kPartyLeaderType));
    }
    catch (...)
    {
    }
}

void ExecuteSlot(void* mgr, std::uint8_t slot) noexcept
{
    const auto* record = static_cast<const std::uint8_t*>(mgr) + kSlotRecords + kSlotRecordSize * (slot & 7);
    const auto  key    = Read<std::uint64_t>(record, 0);
    const auto  owner  = Read<std::uint64_t>(record, 8);
    const int   outer  = t_marksInExecute;
    t_marksInExecute   = 0;
    reinterpret_cast<ExecuteSlotFn>(g_executeSlot.Original())(mgr, slot);
    const int marks  = t_marksInExecute;
    t_marksInExecute = outer;
    if (!Spend())
    {
        return;
    }
    try
    {
        platform::Log(L"interact: execute slot {} key {} (owner {}): leader {}, primary {}; executed {}, pending {}, "
                      L"WorldItem marks {}",
                      slot, Name(key), Name(owner), LeaderText(), PrimaryText(),
                      Read<std::uint8_t>(mgr, kSlotExecuted + (slot & 7)), Read<std::uint8_t>(mgr, kPending), marks);
    }
    catch (...)
    {
    }
}

std::uint8_t MarkByKey(void* items, std::uint64_t key) noexcept
{
    const auto caller = Ida(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    const auto taken  = reinterpret_cast<MarkByKeyFn>(g_markByKey.Original())(items, key);
    if (t_marksInExecute >= 0 && taken != 0)
    {
        ++t_marksInExecute;
    }
    if (Spend())
    {
        try
        {
            platform::Log(L"interact: WorldItem mark {} -> {} ({}, from 0x{:X})", Name(key), taken,
                          caller == kMarkFromListener ? L"the key listener" : L"another caller", caller);
        }
        catch (...)
        {
        }
    }
    return taken;
}

void Acquire(void* items, void* entry) noexcept
{
    const auto key = Read<std::uint64_t>(static_cast<const std::uint8_t*>(entry) - kEntryKeyBack, 0);
    reinterpret_cast<AcquireFn>(g_acquire.Original())(items, entry);
    if (Spend())
    {
        try
        {
            platform::Log(L"interact: WorldItem grant {} (the item-get rumble goes to the primary controller: {})",
                          Name(key), PrimaryText());
        }
        catch (...)
        {
        }
    }
}

// --- After each selection update (game thread) -----------------------------------------------------------

/// The slot block, logged when it changes.
void LogSlots(const std::uint8_t* mgr)
{
    std::array<std::uint8_t, sizeof(g_slotSeen)> now{};
    std::size_t                                  at = 0;
    platform::CopyGuarded(now.data() + at, mgr + kSlotTargets, 8 * kSlots);
    at += 8 * kSlots;
    platform::CopyGuarded(now.data() + at, mgr + kSlotRows, 8 * kSlots);
    at += 8 * kSlots;
    platform::CopyGuarded(now.data() + at, mgr + kSlotHeld, kSlots);
    platform::CopyGuarded(now.data() + at + kSlots, mgr + kSlotPressed, kSlots);
    platform::CopyGuarded(now.data() + at + 2 * kSlots, mgr + kSlotExecuted, kSlots);
    at += 3 * kSlots;
    now[at] = Read<std::uint8_t>(mgr, kPending);
    if (now == g_slotSeen || !Spend())
    {
        return;
    }
    g_slotSeen = now;
    std::wstring slots;
    for (int i = 0; i < kSlots; ++i)
    {
        const auto target = Read<std::uint64_t>(mgr, kSlotTargets + 8 * i);
        const auto row    = Read<std::uint64_t>(mgr, kSlotRows + 8 * i);
        const auto held   = Read<std::uint8_t>(mgr, kSlotHeld + i);
        const auto press  = Read<std::uint8_t>(mgr, kSlotPressed + i);
        const auto done   = Read<std::uint8_t>(mgr, kSlotExecuted + i);
        if (target == 0 && row == 0 && held == 0 && press == 0 && done == 0)
        {
            continue;
        }
        slots += std::format(L"{}[{}] {}{}{}{}{}", slots.empty() ? L"" : L"; ", i, Name(target),
                             row != 0 ? L", row" : L"", held != 0 ? L", held" : L"", press != 0 ? L", pressed" : L"",
                             done != 0 ? L", executed" : L"");
    }
    platform::Log(L"interact: slots {} (candidates {}, pending {}, leader {})", slots.empty() ? L"none" : slots,
                  Read<std::int32_t>(mgr, kCandidateCount), now[at], LeaderText());
}

/// The keys in WorldItemMgr's map (allocated elements only).
std::unordered_set<std::uint64_t> WorldItemKeys(const std::uint8_t* items)
{
    std::unordered_set<std::uint64_t> keys;
    const auto*                       elements = PointerAt(items, kItemElements);
    const auto                        count    = std::clamp(Read<std::int32_t>(items, kItemCount), 0, kMaxItems);
    const auto*                       heapBits = PointerAt(items, kItemBitsHeap);
    const auto* bits = heapBits != nullptr ? heapBits : items + kItemBitsInline;
    if (elements == nullptr)
    {
        return keys;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        const auto word = Read<std::uint32_t>(bits, static_cast<std::size_t>(i / 32) * 4);
        if ((word >> (i % 32) & 1) != 0)
        {
            keys.insert(Read<std::uint64_t>(elements, static_cast<std::size_t>(i) * kItemStride));
        }
    }
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
    nearby.reserve(kMaxPoints);  // nothing allocates while the game's lock is held
    int           walked = 0;
    float         nearestOne = 1e30f, nearestTwo = 1e30f;
    std::uint64_t nearestOneKey = 0, nearestTwoKey = 0;
    {
        const SharedLock lock(reinterpret_cast<PSRWLOCK>(const_cast<std::uint8_t*>(mgr) + kPointLock));
        const auto* node = PointerAt(mgr, kPointList);
        for (int n = 0; node != nullptr && n < kMaxPoints; ++n, node = PointerAt(node, kNodeNext))
        {
            const auto* point = node + kNodePoint;
            float       at[3]{};
            platform::CopyGuarded(at, point + kPointLocation, sizeof(at));
            const float d1 = Distance(at, one), d2 = Distance(at, two);
            ++walked;
            if (d1 < nearestOne)
            {
                nearestOne    = d1;
                nearestOneKey = Read<std::uint64_t>(point, kPointKey);
            }
            if (d2 < nearestTwo)
            {
                nearestTwo    = d2;
                nearestTwoKey = Read<std::uint64_t>(point, kPointKey);
            }
            if (d1 > kNearCm && d2 > kNearCm)
            {
                continue;
            }
            PointSeen seen;
            seen.flags   = Read<std::uint32_t>(point, kPointFlags);
            seen.slot    = Read<std::uint8_t>(point, kPointSlot);
            seen.talk    = Read<std::uint8_t>(point, kPointTalk);
            seen.nearOne = d1 <= kNearCm;
            seen.nearTwo = d2 <= kNearCm;
            nearby.push_back({Read<std::uint64_t>(point, kPointKey), seen, point, d1, d2});
        }
    }
    // A summary every kRegistrarPeriodMs, so an empty census reads as "nothing near", not as a broken walk.
    if (const ULONGLONG now = GetTickCount64(); now >= g_censusSummaryAt && Spend())
    {
        g_censusSummaryAt = now + kRegistrarPeriodMs;
        platform::Log(L"interact: census: {} point(s) registered, {} within 20 m of a player; nearest to player 1 {} at "
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
        if (!Spend())
        {
            continue;  // retried at the next census
        }
        g_points[n.key] = n.seen;
        if (first)
        {
            platform::Log(L"interact: near {} (owner {}, {}, talk {}, presets {} / {} / {}, slot {}, kind {}, variant {}, "
                          L"flags 0x{:08X}): {:.1f} m from player 1, {:.1f} m from player 2",
                          Name(n.key), Name(Read<std::uint64_t>(n.point, kPointOwner)),
                          items.contains(n.key) ? L"WorldItem" : L"not a WorldItem", n.seen.talk,
                          Name(Read<std::uint64_t>(n.point, kPointPreset)),
                          Name(Read<std::uint64_t>(n.point, kPointPreset2)),
                          Name(Read<std::uint64_t>(n.point, kPointPresetPlayer)), n.seen.slot,
                          Read<std::uint8_t>(n.point, kPointKind), Read<std::uint8_t>(n.point, kPointVariant),
                          n.seen.flags, n.d1 / 100.0f, n.d2 / 100.0f);
        }
        else
        {
            platform::Log(L"interact: {} now slot {}, flags 0x{:08X}{}: {:.1f} m from player 1, {:.1f} m from player 2",
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
        if (!Spend())
        {
            ++it;  // retried at the next census
            continue;
        }
        platform::Log(L"interact: {} is no longer near either player (or was removed)", Name(it->first));
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
    RawArray          found{g_found.data(), 0, kMaxPoints};  // the pool's size: the query never reallocates it
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
    const int                  count = std::clamp(found.num, 0, kMaxPoints);
    keys.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        keys.push_back(Read<std::uint64_t>(g_found[i], kPointKey));
    }
    std::sort(keys.begin(), keys.end());
    if (keys == g_dryRunSeen || !Spend())
    {
        return;
    }
    g_dryRunSeen = keys;
    std::wstring line;
    for (int i = 0; i < count; ++i)
    {
        const auto* point = static_cast<const std::uint8_t*>(g_found[i]);
        const auto  key   = Read<std::uint64_t>(point, kPointKey);
        float       at[3]{};
        platform::CopyGuarded(at, point + kPointLocation, sizeof(at));
        line += std::format(L"{}{} ({}, {:.1f} m)", line.empty() ? L"" : L"; ", Name(key),
                            items.contains(key) ? L"WorldItem" : Name(Read<std::uint64_t>(point, kPointPreset)),
                            Distance(at, two) / 100.0f);
    }
    platform::Log(L"interact: dry run around player 2's member: {} candidate(s){}{}", count, count != 0 ? L": " : L"",
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
    if (counts.empty() || !Spend())
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
    platform::Log(L"interact: registrations in the last period by registrar (return address): {}", line);
}

void AfterUpdate(const std::uint8_t* mgr)
{
    LogSlots(mgr);
    LogRegistrars();
    const bool in = generation::Active() && generation::Member() != nullptr;
    if (in != g_wasIn)
    {
        g_wasIn = in;
        g_points.clear();
        g_dryRunSeen.clear();
    }
    const ULONGLONG now = GetTickCount64();
    if (!in || now < g_censusAt)
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

void Update(void* mgr, float dt) noexcept
{
    reinterpret_cast<UpdateFn>(g_update.Original())(mgr, dt);
    if (mgr == nullptr || !generation::OnGameThread())
    {
        return;
    }
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
    g_base           = image.Base();
    const auto root  = image.Rebase(kGameRoot, sizeof(void*));
    const auto party = image.Rebase(kPartyManager, sizeof(void*));
    if (!root || !party || !leader_scope::Install(image))
    {
        platform::Log(L"interact: the root or party manager is outside the image; not watched");
        return;
    }
    g_root  = *root;
    g_party = *party;

    const auto entry = [&](platform::EntryRedirect& redirect, std::uintptr_t ida, std::span<const std::uint8_t> bytes,
                           std::uintptr_t replacement) {
        const auto at = image.Rebase(ida);
        return at && image.BytesMatch(ida, bytes) &&
               redirect.Install(*at, bytes.first(kFirstInstruction), replacement);
    };
    const auto call = [&](platform::CallRedirect& redirect, std::uintptr_t site, std::uintptr_t target,
                          std::uintptr_t replacement) {
        const auto at = image.Rebase(site, 5);
        const auto to = image.Rebase(target);
        return at && to && redirect.Install(*at, *to, replacement);
    };
    const bool add     = entry(g_addPoint, kAddPoint, kAddPointBytes, reinterpret_cast<std::uintptr_t>(&AddPoint));
    const bool execute = entry(g_executeSlot, kExecuteSlot, kExecuteSlotBytes, reinterpret_cast<std::uintptr_t>(&ExecuteSlot));
    const bool mark    = entry(g_markByKey, kMarkByKey, kMarkByKeyBytes, reinterpret_cast<std::uintptr_t>(&MarkByKey));
    const bool acquire = call(g_acquire, kAcquireCall, kAcquire, reinterpret_cast<std::uintptr_t>(&Acquire));
    if (const auto query = image.Rebase(kQuery); query && image.BytesMatch(kQuery, kQueryBytes))
    {
        g_query = reinterpret_cast<QueryFn>(*query);
    }
    const bool update = call(g_update, kUpdateCall, kUpdate, reinterpret_cast<std::uintptr_t>(&Update));
    try
    {
        std::wstring installed, skipped;
        Summary(installed, skipped, L"registrations", add);
        Summary(installed, skipped, L"executions", execute);
        Summary(installed, skipped, L"WorldItem marks", mark);
        Summary(installed, skipped, L"WorldItem grants", acquire);
        Summary(installed, skipped, L"the slots, census and dry run after each update", update);
        Summary(installed, skipped, L"the dry-run query", g_query != nullptr);
        platform::Log(L"interact: watching {}; not watched (the code is not what the research read): {}",
                      installed.empty() ? L"nothing" : installed, skipped.empty() ? L"none" : skipped);
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::interact_watch
