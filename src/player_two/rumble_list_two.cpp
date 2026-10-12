#include "player_two/rumble_list_two.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/chocobo_ride.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::player_two::rumble_list_two {

namespace {

// 1.004 addresses; 1.005 through replaced-rumble-list.json (every function, call and the lock read from both exes).
constexpr std::uintptr_t kPlayNamed     = 0x141302F0C;  // VibrationList3B0_PlayNamed(key, params*, flag)
constexpr std::uint8_t   kPlayNamedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8],rbx (crosses a block in 1.004)
                                              0x48, 0x89, 0x6C, 0x24, 0x10};  // mov [rsp+10h],rbp
constexpr std::size_t    kFirstInstruction  = 5;
constexpr std::size_t    kSecondInstruction = 5;
constexpr std::uintptr_t kAddOrUpdate   = 0x141302E2C;  // VibrationList3B0_AddOrUpdate(list, key, params*, flag)
constexpr std::uintptr_t kListTick      = 0x14125D324;  // Vibration_TickGlobalList_ApplyToPrimaryPC(dt xmm0)
constexpr std::uintptr_t kBuildSet      = 0x14125CFD0;  // Vibration_TickList_BuildChannelSet(list, dt xmm1, set*) -> changed
constexpr std::uintptr_t kApply         = 0x1408A1688;  // PrimaryPC_ApplyVibrationChannels758(list, set*): on PM+0x140
constexpr std::uintptr_t kFreeArray     = 0x14079F8D0;  // the set's data, freed as the list tick frees it
constexpr std::uintptr_t kListCtor      = 0x143727368;  // VibrationList_Ctor (6 channel slots in 1.004, 7 in 1.005)
constexpr std::uintptr_t kMalloc        = 0x14083F3D8;  // FMemory_Malloc(size)
constexpr std::uint8_t   kCtorBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t   kMallocBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kListLock      = 0x14881C428;  // the game's CRITICAL_SECTION around adds and the set build
constexpr std::uintptr_t kRows          = 0x14125D474;  // LeaderActionComp_PerFrameB_RowsAndVibration(comp, xmm1, xmm2)
constexpr std::uint8_t   kRowsBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::uintptr_t kListTickCall  = 0x14125D558;  // ... its call of the list tick
constexpr std::uintptr_t kZipGateCall   = 0x1433251E2;  // Zipline phase 3: the rumble block's player test (rcx = move)
constexpr std::uintptr_t kZipGate       = 0x141635DE8;  // FieldActionMove_IsPlayerChar_Ex_UCPCOrVehicleOrLeaderTag
constexpr std::uintptr_t kZipAddCall    = 0x1433252C6;  // ... the keep-alive add (call PlayNamed)
// RE-p2-rumble-owner-chocobo.md (read from both exes, 2026-10-12).
constexpr std::uintptr_t kStopNamed     = 0x1422C4818;  // VibrationList3B0_StopNamed(key): flags the global list's entries
constexpr std::uint8_t   kStopNamedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8],rbx
                                              0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::size_t    kStopFirstInstruction = 5;
constexpr std::uintptr_t kListOwner     = 0x14939BC90;  // the list's owner (StopNamed and PlayNamed read it)
constexpr std::uintptr_t kFootSlot      = 0x1457F3ED0;  // EndChocoboCharacter vt+0x978 (1.005 +0x980): footsteps
constexpr std::uintptr_t kFootImpact    = 0x1418BA510;  // ... its function: GetMoveState, then FFB_Chocobo_1/2
// The footsteps' gate, read again for the log when player 2's footstep adds nothing: the primary player's coordinator's
// ride status for the rider (2 = riding), the chocobo's gait (> 0), and the primary player's "can start a field action".
constexpr std::uintptr_t kRideStatus    = 0x140A60ABC;  // PrimaryPCModes_AnyVt2A8ForActor_Keys0to5(actor, 0)
constexpr std::uint8_t   kRideStatusBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57};
constexpr std::uintptr_t kCanAct        = 0x140E82AF4;  // PrimaryPC_Char5A0_CanStartFieldAction(_, 1, 1, 1, 1, 1)
constexpr std::uint8_t   kCanActBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10};
constexpr platform::PerBuild<std::uint32_t> kChocoboRider{0x908, 0x910};    // the chocobo's vtable: its rider
constexpr platform::PerBuild<std::uint32_t> kChocoboGait{0x1A20, 0x1A40};   // the chocobo's gait (footsteps when > 0)

// The list object's size, as the global owner's creator allocates it (1.004 `mov ecx,178h` at 0x1436F4AF2, 1.005
// `mov ecx,1B0h` at 0x143429CAA; read from both exes, 2026-10-11).
constexpr platform::PerBuild<std::size_t> kListSize{0x178, 0x1B0};

constexpr std::uint32_t kComponentOwner = 0xA0;   // the leader-action component's controller
constexpr std::uint32_t kListEntries    = 0x8;    // TArray of 48-byte entries
constexpr std::uint32_t kArrayNum       = 0x8;    // a TArray's Num, after its data pointer
constexpr std::uint32_t kMoveCharacter  = 0x450;  // the move's character, an FWeakObjectPtr
constexpr std::uint32_t kOwnerList      = 0x3B0;  // the owner's list pointer
constexpr auto          kSummaryEvery   = std::chrono::seconds(10);

using AddOrUpdateFn = void (*)(void* list, std::uint64_t key, const void* params, std::uint8_t flag);
using PlayNamedFn   = void (*)(std::uint64_t key, const void* params, std::uint8_t flag);
using BuildSetFn    = bool (*)(void* list, float dt, void* set);
using ApplyFn       = void (*)(void* list, void* set);
using FreeFn        = void (*)(void* data);
using CtorFn        = void* (*)(void* memory);
using MallocFn      = void* (*)(std::size_t size);
using DeleteFn      = void* (*)(void* object, std::uint32_t flags);  // vtable slot 0: the scalar deleting destructor
using ListTickFn    = void (*)(float dt);
using RowsFn        = std::uint64_t (*)(void* component, double xmm1, double xmm2);
using ZipGateFn     = bool (*)(void* move);
using StopNamedFn   = void (*)(std::uint64_t key);
using FootFn        = double (*)(void* chocobo);  // returns xmm0 as the game's does
using RideStatusFn  = std::uint8_t (*)(const void* actor, int key);
using CanActFn      = bool (*)(void* unused, bool a, bool b, bool c, bool d, bool e);
using RiderFn       = void* (*)(void* chocobo);

struct ChannelSet  // the game's TArray of 32-byte channel items
{
    void*         data = nullptr;
    std::int32_t  num  = 0;
    std::int32_t  max  = 0;
};

platform::EntryRedirect g_playNamed;
platform::EntryRedirect g_rows;
platform::CallRedirect  g_listTickCall;
platform::CallRedirect  g_zipGateCall;
platform::CallRedirect  g_zipAddCall;
platform::EntryRedirect g_stopNamed;
platform::VTableEntry   g_footSlot;
std::uintptr_t          g_listOwner = 0;  // the global holding the list's owner
RideStatusFn            g_rideStatus = nullptr;  // the footsteps' gate, for the log (null: not logged)
CanActFn                g_canAct     = nullptr;
AddOrUpdateFn           g_addOrUpdate = nullptr;
BuildSetFn              g_buildSet    = nullptr;
ApplyFn                 g_apply       = nullptr;
FreeFn                  g_free        = nullptr;
CtorFn                  g_ctor        = nullptr;
MallocFn                g_malloc      = nullptr;
CRITICAL_SECTION*       g_lock        = nullptr;
bool                    g_installed   = false;

// Player 2's list: written on the game thread under the game's lock, read by adds on any thread under it.
std::atomic<void*>         g_listTwo{nullptr};
std::atomic<std::uint64_t> g_memberWeak{0};  // the zip line's gate compares the move's character with it (any thread)

thread_local const void* t_tickOwner = nullptr;  // the controller whose component's rows tick runs on this thread
thread_local bool        t_zipTwo    = false;    // the zip line's rumble block runs for player 2's member

struct Counts
{
    std::atomic<unsigned> addsOne{0};      // adds left to the game's list
    std::atomic<unsigned> addsTwo{0};      // adds into player 2's list (game thread, player 2 primary)
    std::atomic<unsigned> zipAddsTwo{0};   // ... from player 2's zip line (worker)
    std::atomic<unsigned> zipAddsOne{0};   // the zip line's adds for anyone else
    std::atomic<unsigned> ticksOne{0};     // the game's list ticked by a component (player 1's)
    std::atomic<unsigned> ticksTwo{0};     // player 2's list ticked by player 2's component
    std::atomic<unsigned> appliedTwo{0};   // ... with changes, played on player 2's controller
    std::atomic<unsigned> skippedOne{0};   // the game's list tick skipped inside player 2's primary scope
    std::atomic<unsigned> refused{0};      // player 2's tick without player 2's primary scope (not played)
    std::atomic<unsigned> stopsOne{0};     // stops on the game's list
    std::atomic<unsigned> stopsTwo{0};     // stops on player 2's list (game thread, player 2 primary)
    std::atomic<unsigned> footTwo{0};      // player 2's chocobo's footsteps run in player 2's scope
    std::atomic<unsigned> footRefused{0};  // ... called on the game thread but not run in player 2's scope
    std::atomic<unsigned> footWorkerTwo{0};  // ... called on a worker (the game's gate drops worker calls)
    std::atomic<unsigned> footOther{0};    // every other chocobo's footsteps (game thread)
    std::atomic<unsigned> footWorkerOther{0};  // ... on a worker
};
Counts g_counts;

// The chocobo player 2 drives, for the footstep slot on any thread (the game thread writes it each engine tick).
std::atomic<const void*> g_footChocobo{nullptr};
// The first worker call of player 2's chocobo's footsteps: its return address, logged by the game thread.
std::atomic<std::uintptr_t> g_footWorkerCaller{0};
bool                        g_footWorkerLogged = false;  // game thread

// Game thread.
std::uint64_t                         g_lastKeyTwo  = 0;  // the last effect key added for player 2 (Debug on change)
std::uint64_t                         g_lastStopTwo = 0;  // ... and stopped for player 2
std::chrono::steady_clock::time_point g_summaryAt{};

std::wstring KeyText(std::uint64_t key) noexcept
{
    return players::unreal::NameText(key);
}

class ListLock
{
public:
    ListLock() noexcept { EnterCriticalSection(g_lock); }
    ~ListLock() { LeaveCriticalSection(g_lock); }
    ListLock(const ListLock&)            = delete;
    ListLock& operator=(const ListLock&) = delete;
};

/// Player 2's add, as the game's add does it, into player 2's list. False if player 2 has no list now.
bool AddTwo(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const ListLock lock;
    void* const    list = g_listTwo.load(std::memory_order_acquire);
    if (list == nullptr)
    {
        return false;
    }
    g_addOrUpdate(list, key, params, flag);
    return true;
}

void PlayNamed(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const auto original = reinterpret_cast<PlayNamedFn>(g_playNamed.Original());
    const bool two      = generation::OnGameThread() && generation::Active() &&
                          primary_controller::Is(generation::PlayerTwoController());
    if (!two || !AddTwo(key, params, flag))
    {
        g_counts.addsOne.fetch_add(1, std::memory_order_relaxed);
        original(key, params, flag);
        return;
    }
    g_counts.addsTwo.fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} added to player 2's list", KeyText(key));
    if (key != g_lastKeyTwo)
    {
        g_lastKeyTwo = key;
        platform::LogDebug(L"rumble list: {} ({}) goes to player 2's list (player 2 is the primary player)", KeyText(key),
                           KeyText(params != nullptr ? ReadField<std::uint64_t>(params, 0) : 0));
    }
}

/// Plays player 2's list on player 2's controller, as the game's list tick plays the global one (game thread).
void TickTwo(void* list, float dt) noexcept
{
    ChannelSet set;
    bool       changed = false;
    {
        const ListLock lock;
        changed = g_buildSet(list, dt, &set);
    }
    g_counts.ticksTwo.fetch_add(1, std::memory_order_relaxed);
    if (changed)
    {
        const primary_controller::Scope primary;  // already held inside player 2's scopes
        if (primary.Held())
        {
            g_apply(list, &set);
            g_counts.appliedTwo.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            g_counts.refused.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (set.data != nullptr)
    {
        g_free(set.data);
    }
}

/// The component's list tick: player 2's component ticks player 2's list, player 1's the game's.
void ListTick(float dt) noexcept
{
    const auto original = reinterpret_cast<ListTickFn>(g_listTickCall.Original());
    void* const list    = generation::OnGameThread() && generation::Active() ? g_listTwo.load(std::memory_order_acquire)
                                                                             : nullptr;
    if (list == nullptr)
    {
        original(dt);
        return;
    }
    void* const two = generation::PlayerTwoController();
    if (t_tickOwner != nullptr && t_tickOwner == two)
    {
        TickTwo(list, dt);
        return;
    }
    if (primary_controller::Is(two))
    {
        g_counts.skippedOne.fetch_add(1, std::memory_order_relaxed);  // the game's list never plays on player 2's pad
        return;
    }
    g_counts.ticksOne.fetch_add(1, std::memory_order_relaxed);
    original(dt);
}

/// The leader-action component's rows-and-rumble tick: names its controller for the list tick inside it.
std::uint64_t Rows(void* component, double xmm1, double xmm2) noexcept
{
    const auto  original = reinterpret_cast<RowsFn>(g_rows.Original());
    const void* saved    = t_tickOwner;
    t_tickOwner          = ReadPointer(component, kComponentOwner);
    const auto result    = original(component, xmm1, xmm2);
    t_tickOwner          = saved;
    return result;
}

/// The zip line's rumble test (anim worker): yes for player 2's member, as for player 1's character; the add that
/// follows goes to player 2's list.
bool ZipGate(void* move) noexcept
{
    const auto          original = reinterpret_cast<ZipGateFn>(g_zipGateCall.Original());
    const std::uint64_t member   = g_memberWeak.load(std::memory_order_relaxed);
    t_zipTwo = member != 0 && ReadField<std::uint64_t>(move, kMoveCharacter) == member &&
               g_listTwo.load(std::memory_order_acquire) != nullptr;
    return t_zipTwo || original(move);
}

void ZipAdd(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const bool two = t_zipTwo;
    t_zipTwo       = false;
    if (two && AddTwo(key, params, flag))
    {
        g_counts.zipAddsTwo.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_counts.zipAddsOne.fetch_add(1, std::memory_order_relaxed);
    reinterpret_cast<PlayNamedFn>(g_zipAddCall.Original())(key, params, flag);
}

/// Player 2's stop, as the game's stop does it, on player 2's list: under the game's lock (re-entrant, so the game's own
/// lock inside nests), the owner names player 2's list for the length of the call. Every other reader of the owner's list
/// is under that lock or on this thread (RE-p2-zipline-rumble-hook-who.md §4.2), so no one else sees the swap. False if
/// player 2 has no list now. Game thread.
bool StopTwo(StopNamedFn original, std::uint64_t key) noexcept
{
    const ListLock lock;
    void* const    list  = g_listTwo.load(std::memory_order_acquire);
    void* const    owner = ReadPointer(reinterpret_cast<const void*>(g_listOwner), 0);
    void* const    game  = owner != nullptr ? ReadPointer(owner, kOwnerList) : nullptr;
    if (list == nullptr || game == nullptr)
    {
        return false;
    }
    WriteField<void*>(owner, kOwnerList, list);
    const bool ran = platform::CallGuarded(original, key);  // the owner names the game's list again even if it faults
    WriteField<void*>(owner, kOwnerList, game);
    if (!ran)
    {
        platform::Log(L"WARNING: rumble list: the game's stop faulted on player 2's list ({}); the game's list is "
                      L"restored",
                      platform::DescribeFault(platform::LastFault()).text);
    }
    return true;
}

/// The game's stop by name: player 2's stop goes to player 2's list, by the same rule as the add (game thread, player 2
/// the primary player). Player 2's looping effects (jet, scooter, swimming) were added to player 2's list and never
/// stopped there.
void StopNamed(std::uint64_t key) noexcept
{
    const auto original = reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
    const bool two      = generation::OnGameThread() && generation::Active() &&
                          primary_controller::Is(generation::PlayerTwoController());
    if (!two || !StopTwo(original, key))
    {
        g_counts.stopsOne.fetch_add(1, std::memory_order_relaxed);
        original(key);
        return;
    }
    g_counts.stopsTwo.fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} stopped on player 2's list", KeyText(key));
    if (key != g_lastStopTwo)
    {
        g_lastStopTwo = key;
        platform::LogDebug(L"rumble list: {} stops on player 2's list (player 2 is the primary player)", KeyText(key));
    }
}

// The callers of the chocobo's footstep slot (none is visible statically): the first few, by return address.
std::array<std::uintptr_t, 4> g_footCallers{};  // game thread

void NoteFootCaller(std::uintptr_t returnAddress) noexcept
{
    for (std::uintptr_t& slot : g_footCallers)
    {
        if (slot == returnAddress)
        {
            return;
        }
        if (slot == 0)
        {
            slot             = returnAddress;
            const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            platform::LogDebug(L"rumble list: the chocobo's footsteps are called from 0x{:X} (image +0x{:X})",
                               returnAddress, returnAddress - image);
            return;
        }
    }
}

struct FootCall
{
    FootFn       fn;
    void*        chocobo;
    double       result = 0.0;
    bool         added  = false;  // the footstep went to player 2's list
    // The gate as player 2's scope answers it, read when nothing was added.
    std::uint8_t status = 0;
    std::int32_t gait   = 0;
    bool         canAct = false;
};

// Game thread: the last logged state of player 2's footstep gate (Debug on change).
int g_footState = -1;

void RunFoot(void* context) noexcept
{
    auto&      call   = *static_cast<FootCall*>(context);
    const auto before = g_counts.addsTwo.load(std::memory_order_relaxed);
    call.result       = call.fn(call.chocobo);
    call.added        = g_counts.addsTwo.load(std::memory_order_relaxed) != before;
    if (call.added || g_rideStatus == nullptr || g_canAct == nullptr || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    // Logging only: every game call guarded, a null getter skipped.
    const auto getRider = reinterpret_cast<RiderFn>(ReadPointer(ReadPointer(call.chocobo, 0), kChocoboRider));
    void*      rider    = nullptr;
    if (getRider == nullptr || !platform::CallGuarded(getRider, rider, call.chocobo))
    {
        rider = nullptr;
    }
    if (rider == nullptr || !platform::CallGuarded(g_rideStatus, call.status, static_cast<const void*>(rider), 0))
    {
        call.status = 0;
    }
    call.gait = ReadField<std::int32_t>(call.chocobo, kChocoboGait);
    if (!platform::CallGuarded(g_canAct, call.canAct, static_cast<void*>(nullptr), true, true, true, true, true))
    {
        call.canAct = false;
    }
}

/// Logs a change of player 2's footstep result: added, or which part of the game's gate said no (Debug on change).
void NoteFoot(const FootCall& call) noexcept
{
    const int state = call.added ? 1 : 0x100 | call.status | (call.gait > 0 ? 0x1000 : 0) | (call.canAct ? 0x2000 : 0);
    if (state == g_footState)
    {
        return;
    }
    g_footState = state;
    if (call.added)
    {
        platform::LogDebug(L"rumble list: player 2's chocobo's footsteps add to player 2's list");
        return;
    }
    platform::LogDebug(L"rumble list: player 2's chocobo's footstep added nothing; the game's gate in player 2's scope: "
                       L"ride status {} (2 = riding), gait {}, can start a field action {}",
                       call.status, call.gait, call.canAct ? 1 : 0);
}

/// The chocobo's footstep slot (every chocobo's; game thread). The footsteps of the chocobo player 2 rides run inside
/// player 2's primary scope, as the game runs the chocobo player 1 rides, so the game's own gate asks player 2's
/// coordinator and the add goes to player 2's list. Every other chocobo's passes through.
double FootImpact(void* chocobo) noexcept
{
    const auto original = reinterpret_cast<FootFn>(g_footSlot.Original());
    const bool twos     = chocobo != nullptr && chocobo == g_footChocobo.load(std::memory_order_relaxed);
    if (!generation::OnGameThread())
    {
        // Logging only: the game's gate returns at once on a worker.
        if (twos)
        {
            g_counts.footWorkerTwo.fetch_add(1, std::memory_order_relaxed);
            std::uintptr_t none = 0;
            g_footWorkerCaller.compare_exchange_strong(none, reinterpret_cast<std::uintptr_t>(_ReturnAddress()),
                                                       std::memory_order_relaxed);
        }
        else
        {
            g_counts.footWorkerOther.fetch_add(1, std::memory_order_relaxed);
        }
        return original(chocobo);
    }
    if (platform::Enabled(platform::Level::Debug))
    {
        NoteFootCaller(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    }
    FootCall call{original, chocobo};
    if (!twos)
    {
        g_counts.footOther.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_listTwo.load(std::memory_order_acquire) != nullptr && chocobo_ride::RunAsRider(chocobo, &RunFoot, &call))
    {
        g_counts.footTwo.fetch_add(1, std::memory_order_relaxed);
        platform::LogTrace(L"rumble list: player 2's chocobo's footsteps ran in player 2's scope (added {})",
                           call.added ? 1 : 0);
        if (platform::Enabled(platform::Level::Debug))
        {
            NoteFoot(call);
        }
        return call.result;
    }
    if (twos)
    {
        g_counts.footRefused.fetch_add(1, std::memory_order_relaxed);
        platform::LogTrace(L"rumble list: player 2's chocobo's footstep not run in player 2's scope (ridden {})",
                           chocobo_ride::RiddenChocobo() == chocobo ? 1 : 0);
    }
    return original(chocobo);
}

/// Game thread: player 2's list, made by the game's allocator and constructor.
void MakeList() noexcept
{
    void* const list = g_malloc(kListSize);
    if (list != nullptr)
    {
        g_ctor(list);
    }
    if (list == nullptr)
    {
        platform::Log(L"WARNING: rumble list: player 2's list could not be made; the game's list plays on the primary "
                      L"player's pad");
        return;
    }
    {
        const ListLock lock;
        g_listTwo.store(list, std::memory_order_release);
    }
    g_lastKeyTwo = 0;
    platform::Log(L"rumble list: player 2 has their own rumble list (traversal rumble per pad)");
}

/// Game thread: stops player 2's channels (when player 2 can still be the primary player) and destroys the list.
void DropList(const wchar_t* why) noexcept
{
    if (g_listTwo.load(std::memory_order_acquire) == nullptr)
    {
        return;  // only this thread makes or drops the list
    }
    void* list = nullptr;
    {
        const ListLock lock;
        list = g_listTwo.exchange(nullptr, std::memory_order_acq_rel);
        if (list != nullptr)
        {
            WriteField<std::int32_t>(list, kListEntries + kArrayNum, 0);  // as the game's own clear
        }
    }
    if (list == nullptr)
    {
        return;
    }
    bool stopped = false;
    if (generation::Active())
    {
        const primary_controller::Scope primary;
        if (primary.Held())
        {
            ChannelSet none;
            g_apply(list, &none);  // every channel of player 2's list stops on player 2's controller
            stopped = true;
        }
    }
    const auto destroy = reinterpret_cast<DeleteFn>(ReadPointer(ReadPointer(list, 0), 0));
    if (destroy != nullptr)
    {
        destroy(list, 1);
    }
    platform::Log(L"rumble list: player 2's rumble list removed ({}; its channels {})", why,
                  stopped ? L"stopped on player 2's controller" : L"not stopped: player 2 is no longer the primary player");
}

void LogSummary() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_summaryAt < kSummaryEvery)
    {
        return;
    }
    g_summaryAt     = now;
    const auto a1   = g_counts.addsOne.exchange(0, std::memory_order_relaxed);
    const auto a2   = g_counts.addsTwo.exchange(0, std::memory_order_relaxed);
    const auto z2   = g_counts.zipAddsTwo.exchange(0, std::memory_order_relaxed);
    const auto z1   = g_counts.zipAddsOne.exchange(0, std::memory_order_relaxed);
    const auto t1   = g_counts.ticksOne.exchange(0, std::memory_order_relaxed);
    const auto t2   = g_counts.ticksTwo.exchange(0, std::memory_order_relaxed);
    const auto p2   = g_counts.appliedTwo.exchange(0, std::memory_order_relaxed);
    const auto skip = g_counts.skippedOne.exchange(0, std::memory_order_relaxed);
    const auto ref  = g_counts.refused.exchange(0, std::memory_order_relaxed);
    const auto s1   = g_counts.stopsOne.exchange(0, std::memory_order_relaxed);
    const auto s2   = g_counts.stopsTwo.exchange(0, std::memory_order_relaxed);
    const auto foot = g_counts.footTwo.exchange(0, std::memory_order_relaxed);
    const auto footRefused     = g_counts.footRefused.exchange(0, std::memory_order_relaxed);
    const auto footWorkerTwo   = g_counts.footWorkerTwo.exchange(0, std::memory_order_relaxed);
    const auto footOther       = g_counts.footOther.exchange(0, std::memory_order_relaxed);
    const auto footWorkerOther = g_counts.footWorkerOther.exchange(0, std::memory_order_relaxed);
    if (a1 + a2 + z1 + z2 + t1 + t2 == 0 || g_listTwo.load(std::memory_order_relaxed) == nullptr)
    {
        return;
    }
    platform::LogDebug(L"rumble list ({} s): adds to the game's list {} (zip line {}), to player 2's {} (zip line {}); "
                       L"stops on the game's list {}, on player 2's {}; player 2's chocobo footsteps in player 2's scope "
                       L"{} (not in it {}, on a worker {}), other chocobos' footsteps {} (on a worker {}); ticks of the "
                       L"game's list {}, of player 2's {} (played on player 2's controller {}, refused {}); the game's "
                       L"list skipped inside player 2's scope {}",
                       kSummaryEvery.count(), a1, z1, a2 + z2, z2, s1, s2, foot, footRefused, footWorkerTwo, footOther,
                       footWorkerOther, t1, t2, p2, ref, skip);
}

bool Call(const platform::GameImage& image, platform::CallRedirect& redirect, std::uintptr_t site,
          std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(site, 5);
    const auto to = image.Rebase(target);
    return at && to && redirect.Install(*at, *to, replacement);
}

template <class Fn>
bool Find(const platform::GameImage& image, std::uintptr_t address, Fn& out) noexcept
{
    const auto at = image.Rebase(address);
    out           = at ? reinterpret_cast<Fn>(*at) : nullptr;
    return out != nullptr;
}

void RemoveRedirects() noexcept
{
    for (platform::CallRedirect* call : {&g_zipAddCall, &g_zipGateCall, &g_listTickCall})
    {
        if (call->IsInstalled())
        {
            call->Restore();
        }
    }
    if (g_footSlot.Original() != 0)
    {
        g_footSlot.Restore();
    }
    for (platform::EntryRedirect* entry : {&g_stopNamed, &g_rows, &g_playNamed})
    {
        if (entry->IsInstalled())
        {
            entry->Restore();
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto lock = image.Rebase(kListLock, sizeof(CRITICAL_SECTION));
    g_lock          = lock ? reinterpret_cast<CRITICAL_SECTION*>(*lock) : nullptr;
    const bool functions = g_lock != nullptr && Find(image, kAddOrUpdate, g_addOrUpdate) &&
                           Find(image, kBuildSet, g_buildSet) && Find(image, kApply, g_apply) &&
                           Find(image, kFreeArray, g_free) && image.BytesMatch(kListCtor, kCtorBytes) &&
                           Find(image, kListCtor, g_ctor) && image.BytesMatch(kMalloc, kMallocBytes) &&
                           Find(image, kMalloc, g_malloc);
    // The add's entry: its first instruction crosses a block in 1.004, so the second is replaced there.
    bool                                add = false;
    const std::span<const std::uint8_t> bytes(kPlayNamedBytes);
    const auto                          playAt = image.Rebase(kPlayNamed);
    if (functions && playAt && image.BytesMatch(kPlayNamed, bytes))
    {
        const auto replacement = reinterpret_cast<std::uintptr_t>(&PlayNamed);
        add = g_playNamed.Install(*playAt, image.EntryInstruction(kPlayNamed, bytes.first(kFirstInstruction)),
                                  replacement) ||
              g_playNamed.InstallAtSecondInstruction(*playAt, bytes.first(kFirstInstruction),
                                                     bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
    }
    const auto rowsAt = image.Rebase(kRows);
    const bool rows   = add && rowsAt && image.BytesMatch(kRows, kRowsBytes) &&
                      g_rows.Install(*rowsAt, image.EntryInstruction(kRows, kRowsBytes),
                                     reinterpret_cast<std::uintptr_t>(&Rows));
    const bool tick = rows && Call(image, g_listTickCall, kListTickCall, kListTick,
                                   reinterpret_cast<std::uintptr_t>(&ListTick));
    const bool zip  = tick &&
                     Call(image, g_zipAddCall, kZipAddCall, kPlayNamed, reinterpret_cast<std::uintptr_t>(&ZipAdd)) &&
                     Call(image, g_zipGateCall, kZipGateCall, kZipGate, reinterpret_cast<std::uintptr_t>(&ZipGate));
    // The stop by name: player 2's loops added to player 2's list must stop there.
    const auto owner  = image.Rebase(kListOwner, sizeof(void*));
    g_listOwner       = owner ? *owner : 0;
    const auto stopAt = image.Rebase(kStopNamed);
    const bool stop   = zip && g_listOwner != 0 && stopAt && image.BytesMatch(kStopNamed, kStopNamedBytes) &&
                      g_stopNamed.Install(*stopAt,
                                          image.EntryInstruction(kStopNamed,
                                                                 std::span(kStopNamedBytes).first(kStopFirstInstruction)),
                                          reinterpret_cast<std::uintptr_t>(&StopNamed));
    g_installed = stop;
    if (!g_installed)
    {
        RemoveRedirects();  // all or none: an add into player 2's list needs the tick that plays it and the stop
    }
    // The chocobo's footsteps (optional: without it, player 2's footsteps play on the primary player's pad as before).
    const auto footSlot = image.Rebase(kFootSlot, sizeof(void*));
    const auto foot     = image.Rebase(kFootImpact);
    const bool feet     = g_installed && footSlot && foot &&
                      g_footSlot.Replace(*footSlot, *foot, reinterpret_cast<std::uintptr_t>(&FootImpact));
    if (!(image.BytesMatch(kRideStatus, kRideStatusBytes) && Find(image, kRideStatus, g_rideStatus) &&
          image.BytesMatch(kCanAct, kCanActBytes) && Find(image, kCanAct, g_canAct)))
    {
        g_rideStatus = nullptr;  // the footsteps' gate is not logged
        g_canAct     = nullptr;
    }
    platform::Log(L"rumble list: {}{}",
                  g_installed ? L"each player's traversal rumble (zip line, chocobo, ropes, gimmicks) plays and stops on "
                                L"their own pad"
                              : L"UNCHANGED, the game's one list plays on the primary player's pad (a function or call "
                                L"is not what the research read)",
                  !g_installed ? L""
                  : feet       ? L"; player 2's chocobo's footsteps run in player 2's scope"
                               : L"; the chocobo's footstep slot is not what the research read: player 2's footsteps stay "
                                 L"on the primary player's pad");
}

void Remove() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        DropList(L"the mod is removed");
    }
    else if (g_listTwo.load(std::memory_order_acquire) != nullptr)
    {
        platform::Log(L"WARNING: rumble list: removed off the game thread; player 2's list is left allocated");
    }
    g_installed = false;
    RemoveRedirects();
}

void Tick() noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        g_memberWeak.store(0, std::memory_order_relaxed);
        g_footChocobo.store(nullptr, std::memory_order_relaxed);
        DropList(L"player 2 is out");
        return;
    }
    g_footChocobo.store(chocobo_ride::DrivenChocobo(), std::memory_order_relaxed);
    const auto workerCaller = g_footWorkerCaller.load(std::memory_order_relaxed);
    if (workerCaller != 0 && !g_footWorkerLogged)
    {
        g_footWorkerLogged = true;
        const auto image   = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        platform::LogDebug(L"rumble list: player 2's chocobo's footsteps are called on a worker, from 0x{:X} (image "
                           L"+0x{:X}); the game's gate drops worker calls",
                           workerCaller, workerCaller - image);
    }
    void* const member = generation::Member();
    g_memberWeak.store(member != nullptr ? leader_scope::WeakOf(member) : 0, std::memory_order_relaxed);
    if (g_listTwo.load(std::memory_order_relaxed) == nullptr && generation::PlayerTwoController() != nullptr)
    {
        MakeList();
    }
    LogSummary();
}

void PlayerTwoOut() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        g_memberWeak.store(0, std::memory_order_relaxed);
        DropList(L"player 2 leaves");
    }
}

}  // namespace lss::player_two::rumble_list_two
