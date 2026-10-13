#include "player_two/rumble_list_two.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/chocobo_ride.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

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
// The rumble sources that run outside player 2's scopes (RE-p2-rumble-owner-chocobo.md §9; slots and code read from
// both exes, 2026-10-12). Each names player 2 as the owner of its adds while it runs for player 2's scooter or chocobo.
constexpr std::uintptr_t kScooterTickSlot = 0x1466B9D90;  // SBScooterMoveState slot 1: hit and loop rumble (actor tick)
constexpr std::uintptr_t kScooterTick     = 0x14344F38C;
constexpr std::uintptr_t kScooterOffSlot  = 0x1466B9DC8;  // ... slot 8, Deactivate: stops the loop
constexpr std::uintptr_t kScooterOff      = 0x14344A350;
constexpr std::uintptr_t kGlideSafeSlot   = 0x145CE5C48;  // the sky-glide rumble delegate's ExecuteIfSafe (+0x50, Broadcast)
constexpr std::uintptr_t kGlideSafe       = 0x142247D60;
constexpr std::uintptr_t kGlideRunSlot    = 0x145CE5C40;  // ... its Execute (+0x48)
constexpr std::uintptr_t kGlideRun        = 0x142247D30;
constexpr std::uintptr_t kJumpEnded       = 0x1422ADF00;  // the jump box's OnEnded TFunction invoker (often the worker)
constexpr std::uint8_t   kJumpEndedBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00};  // mov eax,28h
// 1.004 only: the jump box's add is inlined there; this is its list load, under the game's lock (1.005 calls PlayNamed).
constexpr std::uintptr_t kJumpListLoad    = 0x1422AD981;
constexpr std::uint8_t   kJumpListLoadBytes[] = {0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00};  // mov rsi,[rsi+3B0h]
constexpr std::uintptr_t kDriftSlot       = 0x146692AF0;  // the drift action's state 1 tick (state table; anim worker)
constexpr std::uintptr_t kDrift           = 0x14328EFA4;
constexpr std::uintptr_t kBrakeOneSlot    = 0x1470FCDE0;  // the brake move's phase 1 and 2 (phase table; anim worker)
constexpr std::uintptr_t kBrakeOne        = 0x14328B9FC;
constexpr std::uintptr_t kBrakeTwoSlot    = 0x1470FCDE8;
constexpr std::uintptr_t kBrakeTwo        = 0x14328A1A0;
constexpr std::uint32_t  kStateOwner      = 0x8;    // the scooter move state's scooter; the glide state's move owner
constexpr std::uint32_t  kOwnerWeak       = 0x8;    // ... whose weak at +8 is the chocobo
constexpr std::uint32_t  kGlideState      = 0x18;   // the glide delegate instance's captured glide state
constexpr std::uint32_t  kJumpWeak        = 0x8;    // the jump box payload's weak character (the chocobo)
constexpr std::uint32_t  kDriftChocobo    = 0x4B0;  // the drift action's chocobo, a weak

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
using ScooterTickFn = std::uint64_t (*)(void* state, float dt);  // dt in xmm1
using ScooterOffFn  = std::uint64_t (*)(void* state);
using GlideSafeFn   = bool (*)(void* instance);
using GlideRunFn    = void (*)(void* instance);
using JumpEndedFn   = void (*)(void* payload);
using DriftFn       = std::uint64_t (*)(void* action, double dt);  // dt in xmm1
using BrakeFn       = std::uint64_t (*)(void* move, std::uint64_t event, double xmm2, double xmm3);

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
platform::VTableEntry   g_scooterTickSlot;
platform::VTableEntry   g_scooterOffSlot;
platform::VTableEntry   g_glideSafeSlot;
platform::VTableEntry   g_glideRunSlot;
platform::EntryRedirect g_jumpEnded;
platform::CodeRedirect  g_jumpListLoad;  // 1.004 only
platform::VTableEntry   g_driftSlot;
platform::VTableEntry   g_brakeOneSlot;
platform::VTableEntry   g_brakeTwoSlot;
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

// A shared ride (player 1 rides the buggy or the bronco, player 2 stands aside): adds and stops on the game's list are
// copied into player 2's list, so player 2's pad plays what player 1's does. Set by the game thread; read anywhere.
std::atomic<bool> g_copyOne{false};
// The keys copied and not yet stopped there, under the game's lock (the ride's end stops what is left).
std::array<std::uint64_t, 16> g_copiedKeys{};
std::size_t                   g_copiedCount = 0;

// Player 2's chocobo (weak) and their member's player type, for the sources on any thread (written each engine tick).
std::atomic<std::uint64_t> g_chocoboWeak{0};
std::atomic<int>           g_memberType{-1};
// 1.004's jump box: the thread whose inline add takes player 2's list at its list load (0: none). Read by the code at
// the load, under the game's lock, against the running thread's id.
std::atomic<std::uint32_t> g_jumpThread{0};
static_assert(sizeof(std::atomic<void*>) == sizeof(void*) && std::atomic<void*>::is_always_lock_free,
              "the load's code reads g_listTwo as a plain pointer");
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

thread_local const void* t_tickOwner = nullptr;  // the controller whose component's rows tick runs on this thread
thread_local bool        t_zipTwo    = false;    // the zip line's rumble block runs for player 2's member
thread_local bool        t_ownerTwo  = false;    // this thread runs one of player 2's rumble sources (OwnerTwo)

/// Names player 2 as the owner of this thread's adds and stops while it lives (nesting keeps the outer owner).
class OwnerTwo
{
public:
    OwnerTwo() noexcept : saved_(t_ownerTwo) { t_ownerTwo = true; }
    ~OwnerTwo() { t_ownerTwo = saved_; }
    OwnerTwo(const OwnerTwo&)            = delete;
    OwnerTwo& operator=(const OwnerTwo&) = delete;

private:
    bool saved_;
};

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
    // The sources run as player 2's (OwnerTwo), by source, and what their adds and stops did.
    std::atomic<unsigned> scooterTwo{0};   // player 2's scooter's slot 1 or 8
    std::atomic<unsigned> glideTwo{0};     // the glide rumble of player 2's chocobo
    std::atomic<unsigned> jumpTwo{0};      // the jump box of player 2's chocobo
    std::atomic<unsigned> driftTwo{0};     // player 2's chocobo's drift (state 1 ticks)
    std::atomic<unsigned> brakeTwo{0};     // ... and brake (phase calls)
    std::atomic<unsigned> ownerAddsTwo{0};   // adds into player 2's list from those sources
    std::atomic<unsigned> ownerStopsTwo{0};  // stops on player 2's list from them (game thread)
    std::atomic<unsigned> ownerStopsLeft{0};  // ... on a worker: left (no game path stops a list there)
    std::atomic<unsigned> sourceWorker{0};   // source calls for player 2 on a thread other than the game thread
    std::atomic<unsigned> actionsOnGame{0};  // drift and brake calls for player 2 on the game thread (expected 0)
    // A shared ride: the game's list copied into player 2's list.
    std::atomic<unsigned> copiesTwo{0};      // adds copied
    std::atomic<unsigned> copyStopsTwo{0};   // stops copied (game thread)
    std::atomic<unsigned> copyStopsLeft{0};  // stops on a worker: not copied
};
Counts g_counts;

// The chocobo player 2 drives, for the footstep slot on any thread (the game thread writes it each engine tick).
std::atomic<const void*> g_footChocobo{nullptr};
// The first worker call of player 2's chocobo's footsteps: its return address, logged by the game thread.
std::atomic<std::uintptr_t> g_footWorkerCaller{0};
bool                        g_footWorkerLogged = false;  // game thread

// Game thread.
std::atomic<std::uint64_t>            g_lastKeyTwo{0};    // the last effect key added for player 2 (Debug on change)
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

/// A shared ride's copy of an add to the game's list, into player 2's list (any thread, under the game's lock). The key
/// is kept until its stop is copied too, so the ride's end can stop what is left.
void CopyTwo(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const ListLock lock;
    void* const    list = g_listTwo.load(std::memory_order_acquire);
    // Read again under the lock: the ride's end clears the flag before it takes the lock to stop what was copied, so
    // a copy that saw the flag set outside the lock cannot land after that cleanup (and play on with nothing to stop it).
    if (list == nullptr || !g_copyOne.load(std::memory_order_acquire))
    {
        return;
    }
    g_addOrUpdate(list, key, params, flag);
    g_counts.copiesTwo.fetch_add(1, std::memory_order_relaxed);
    const auto end = g_copiedKeys.begin() + g_copiedCount;
    if (std::find(g_copiedKeys.begin(), end, key) == end && g_copiedCount < g_copiedKeys.size())
    {
        g_copiedKeys[g_copiedCount++] = key;
        platform::LogDebug(L"rumble list: shared ride: {} copied into player 2's list", KeyText(key));
    }
}

/// The add by name: player 2's add goes to player 2's list. It is player 2's when its source names player 2 (OwnerTwo,
/// any thread), or on the game thread while player 2 is the primary player (player 2's scopes). During a shared ride
/// an add to the game's list is copied into player 2's list too.
void PlayNamed(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const auto original = reinterpret_cast<PlayNamedFn>(g_playNamed.Original());
    const bool owner    = t_ownerTwo;
    const bool two      = owner || (generation::OnGameThread() && generation::Active() &&
                                    primary_controller::Is(generation::PlayerTwoController()));
    if (!two || !AddTwo(key, params, flag))
    {
        g_counts.addsOne.fetch_add(1, std::memory_order_relaxed);
        original(key, params, flag);
        if (!two && g_copyOne.load(std::memory_order_acquire))
        {
            CopyTwo(key, params, flag);
        }
        return;
    }
    (owner ? g_counts.ownerAddsTwo : g_counts.addsTwo).fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} added to player 2's list", KeyText(key));
    if (g_lastKeyTwo.exchange(key, std::memory_order_relaxed) != key)
    {
        platform::LogDebug(L"rumble list: {} ({}) goes to player 2's list ({})", KeyText(key),
                           KeyText(params != nullptr ? ReadField<std::uint64_t>(params, 0) : 0),
                           owner ? L"its source is player 2's" : L"player 2 is the primary player");
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

// A list entry (0x30 bytes, both builds; RE-p2-ffb-manager-per-pc.md §11, S-RUMSEED) and the add's params (0x20).
constexpr std::uint32_t kEntrySize     = 0x30;
constexpr std::uint32_t kEntryFlags    = 0x00;  // u8: 0x02 loop, 0x04 finished, 0x08 stop requested, 0x10 fading out
constexpr std::uint32_t kEntryState    = 0x01;  // u8: 6 stop, 7 fade-out
constexpr std::uint32_t kEntrySlot     = 0x02;  // u8: the channel slot
constexpr std::uint32_t kEntryDuration = 0x10;  // f32
constexpr std::uint32_t kEntryEffect   = 0x18;  // FName
constexpr std::uint32_t kEntryHold     = 0x20;  // FName: the hold-point key
constexpr std::uint32_t kEntryKey      = 0x28;  // FName
constexpr std::uint8_t  kFlagLoop      = 0x02;
constexpr std::uint8_t  kFlagEnding    = 0x04 | 0x08 | 0x10;
constexpr std::int32_t  kMaxEntries    = 64;    // a sanity cap for the walk (a ride holds a handful)

struct AddParams
{
    std::uint64_t effect   = 0;
    std::uint64_t hold     = 0;
    std::uint8_t  slot     = 0;
    std::uint8_t  padSlot[3]{};     // named, so the struct handed to the game is zeroed whole
    float         duration = 0.0f;  // +0x14
    std::uint8_t  loop     = 0;     // +0x18
    std::uint8_t  padLoop[7]{};
};
static_assert(offsetof(AddParams, slot) == 0x10 && offsetof(AddParams, duration) == 0x14 &&
              offsetof(AddParams, loop) == 0x18 && sizeof(AddParams) == 0x20);

/// A list's entries (data, count capped), or none.
std::span<const std::uint8_t> Entries(const void* list) noexcept
{
    const auto* data  = static_cast<const std::uint8_t*>(ReadPointer(list, kListEntries));
    const auto  count = list != nullptr ? ReadField<std::int32_t>(list, kListEntries + kArrayNum) : 0;
    if (data == nullptr || count <= 0 || count > kMaxEntries)
    {
        return {};
    }
    return {data, static_cast<std::size_t>(count) * kEntrySize};
}

bool HasKey(const void* list, std::uint64_t key) noexcept
{
    const auto entries = Entries(list);
    for (std::size_t at = 0; at < entries.size(); at += kEntrySize)
    {
        if (ReadField<std::uint64_t>(entries.data() + at, kEntryKey) == key)
        {
            return true;
        }
    }
    return false;
}

/// A shared ride's copy turns on (game thread, under the game's lock, with the flag set inside it so no add falls
/// between): the loops already playing on the game's list are added to player 2's with the game's own add, params
/// rebuilt from the entry, flag 1 (the bronco loop's own). A loop added before the copy began (player 2 joined into a
/// ride) would otherwise stay off pad 2 until the game re-adds it. Skipped: one-shots (they would replay from the start),
/// entries already stopping or ending (their stop has run; a copy would never stop), and keys player 2's list has.
/// Returns how many were seeded; their names go to `names`.
std::size_t SeedTwo(std::array<std::uint64_t, 16>& names) noexcept
{
    const ListLock lock;
    void* const    two   = g_listTwo.load(std::memory_order_acquire);
    void* const    owner = ReadPointer(reinterpret_cast<const void*>(g_listOwner), 0);
    void* const    game  = owner != nullptr ? ReadPointer(owner, kOwnerList) : nullptr;
    g_copyOne.store(two != nullptr, std::memory_order_release);
    std::size_t seeded = 0;
    if (two == nullptr || game == nullptr || game == two)
    {
        return seeded;
    }
    const auto entries = Entries(game);
    for (std::size_t at = 0; at < entries.size() && seeded < names.size(); at += kEntrySize)
    {
        const std::uint8_t* entry = entries.data() + at;
        const auto          flags = ReadField<std::uint8_t>(entry, kEntryFlags);
        const auto          state = ReadField<std::uint8_t>(entry, kEntryState);
        const auto          key   = ReadField<std::uint64_t>(entry, kEntryKey);
        if ((flags & kFlagLoop) == 0 || (flags & kFlagEnding) != 0 || state == 6 || state == 7 || HasKey(two, key))
        {
            continue;
        }
        AddParams params;
        params.effect   = ReadField<std::uint64_t>(entry, kEntryEffect);
        params.hold     = ReadField<std::uint64_t>(entry, kEntryHold);
        params.slot     = ReadField<std::uint8_t>(entry, kEntrySlot);
        params.duration = ReadField<float>(entry, kEntryDuration);
        params.loop     = 1;
        g_addOrUpdate(two, key, &params, 1);
        names[seeded++] = key;
        const auto end  = g_copiedKeys.begin() + g_copiedCount;
        if (std::find(g_copiedKeys.begin(), end, key) == end && g_copiedCount < g_copiedKeys.size())
        {
            g_copiedKeys[g_copiedCount++] = key;
        }
    }
    return seeded;
}

/// The key leaves the shared ride's copied keys (its stop was copied).
void ForgetCopied(std::uint64_t key) noexcept
{
    const ListLock lock;
    const auto     end = g_copiedKeys.begin() + g_copiedCount;
    const auto     at  = std::find(g_copiedKeys.begin(), end, key);
    if (at != end)
    {
        *at = g_copiedKeys[--g_copiedCount];
    }
}

/// A shared ride's copy of a stop on the game's list, on player 2's list (game thread: the owner's list is named only
/// there).
void CopyStopTwo(StopNamedFn original, std::uint64_t key, bool game) noexcept
{
    if (!game)
    {
        g_counts.copyStopsLeft.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (StopTwo(original, key))
    {
        g_counts.copyStopsTwo.fetch_add(1, std::memory_order_relaxed);
        ForgetCopied(key);
        platform::LogDebug(L"rumble list: shared ride: {} stopped on player 2's list too", KeyText(key));
    }
}

/// The game's stop by name: player 2's stop goes to player 2's list, by the same rule as the add (game thread, player 2
/// the primary player). Player 2's looping effects (jet, scooter, swimming) were added to player 2's list and never
/// stopped there. During a shared ride a stop on the game's list is copied onto player 2's list too.
void StopNamed(std::uint64_t key) noexcept
{
    const auto original = reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
    const bool owner    = t_ownerTwo;
    const bool game     = generation::OnGameThread();
    if (owner && !game && g_listTwo.load(std::memory_order_acquire) != nullptr)
    {
        // A source of player 2's stopping on a worker: the owner's list can be named only on the game thread, and no
        // source found stops there (RE-p2-rumble-owner-chocobo.md §9.4). Player 1's entries are left alone.
        if (g_counts.ownerStopsLeft.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            platform::Log(L"rumble list: a stop of player 2's ({}) ran on a worker; player 1's list was left alone "
                          L"(counted in the Debug summary)",
                          KeyText(key));
        }
        return;
    }
    const bool two = owner || (game && generation::Active() && primary_controller::Is(generation::PlayerTwoController()));
    if (!two || !game || !StopTwo(original, key))
    {
        g_counts.stopsOne.fetch_add(1, std::memory_order_relaxed);
        original(key);
        if (!two && g_copyOne.load(std::memory_order_acquire))
        {
            CopyStopTwo(original, key, game);
        }
        return;
    }
    (owner ? g_counts.ownerStopsTwo : g_counts.stopsTwo).fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} stopped on player 2's list", KeyText(key));
    if (key != g_lastStopTwo)
    {
        g_lastStopTwo = key;
        platform::LogDebug(L"rumble list: {} stops on player 2's list ({})", KeyText(key),
                           owner ? L"its source is player 2's" : L"player 2 is the primary player");
    }
}

/// Player 2's chocobo, by the weak the game keeps for it (any thread).
bool ChocoboTwo(std::uint64_t weak) noexcept
{
    const std::uint64_t two = g_chocoboWeak.load(std::memory_order_relaxed);
    return two != 0 && weak == two && g_listTwo.load(std::memory_order_acquire) != nullptr;
}

/// Counts a source run as player 2's, and whether it ran off the game thread.
void NoteSource(std::atomic<unsigned>& count) noexcept
{
    count.fetch_add(1, std::memory_order_relaxed);
    if (!generation::OnGameThread())
    {
        g_counts.sourceWorker.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Drift and brake are expected on the anim worker: the per-thread leader answer is then the worker's alone. Counted
/// when one runs on the game thread instead (RE-p2-rumble-owner-chocobo.md §9.5, thread ASSUMPTION).
void NoteAction() noexcept
{
    if (generation::OnGameThread())
    {
        g_counts.actionsOnGame.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Player 2's scooter: its move state's scooter is possessed by player 2's controller (any thread).
bool ScooterTwo(const void* state) noexcept
{
    const void* const two     = generation::PlayerTwoController();
    const void* const scooter = two != nullptr ? ReadPointer(state, kStateOwner) : nullptr;
    return scooter != nullptr && ReadPointer(scooter, facts::kPawnController) == two &&
           g_listTwo.load(std::memory_order_acquire) != nullptr;
}

/// The scooter move state's slot 1 (every scooter's, from its actor tick, outside every scope of the mod): player 2's
/// scooter's hit and loop rumble, and the loop's stop, are player 2's.
std::uint64_t ScooterTick(void* state, float dt) noexcept
{
    const auto original = reinterpret_cast<ScooterTickFn>(g_scooterTickSlot.Original());
    if (!ScooterTwo(state))
    {
        return original(state, dt);
    }
    NoteSource(g_counts.scooterTwo);
    const OwnerTwo owner;
    return original(state, dt);
}

/// ... and its slot 8 (Deactivate), which stops the loop.
std::uint64_t ScooterOff(void* state) noexcept
{
    const auto original = reinterpret_cast<ScooterOffFn>(g_scooterOffSlot.Original());
    if (!ScooterTwo(state))
    {
        return original(state);
    }
    NoteSource(g_counts.scooterTwo);
    const OwnerTwo owner;
    return original(state);
}

/// The sky glide's rumble delegate (the move owner's "back to normal movement" broadcast): player 2's chocobo's is
/// player 2's. The chocobo is the weak at +8 of the glide state's move owner.
bool GlideTwo(void* instance) noexcept
{
    const void* const state = ReadPointer(instance, kGlideState);
    const void* const mover = state != nullptr ? ReadPointer(state, kStateOwner) : nullptr;
    return mover != nullptr && ChocoboTwo(ReadField<std::uint64_t>(mover, kOwnerWeak));
}

bool GlideSafe(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideSafeFn>(g_glideSafeSlot.Original());
    if (!GlideTwo(instance))
    {
        return original(instance);
    }
    NoteSource(g_counts.glideTwo);
    const OwnerTwo owner;
    return original(instance);
}

void GlideRun(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideRunFn>(g_glideRunSlot.Original());
    if (!GlideTwo(instance))
    {
        original(instance);
        return;
    }
    NoteSource(g_counts.glideTwo);
    const OwnerTwo owner;
    original(instance);
}

/// 1.004: names this thread for the jump box's inline list load while it lives (the code there takes player 2's list).
class JumpRoute
{
public:
    JumpRoute() noexcept
    {
        std::uint32_t none = 0;
        named_ = g_jumpListLoad.IsInstalled() &&
                 g_jumpThread.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed);
    }
    ~JumpRoute()
    {
        if (named_)
        {
            g_jumpThread.store(0, std::memory_order_relaxed);
        }
    }
    JumpRoute(const JumpRoute&)            = delete;
    JumpRoute& operator=(const JumpRoute&) = delete;

private:
    bool named_ = false;
};

/// The jump box's OnEnded (the jump action's finish or abort): player 2's chocobo's landing rumble is player 2's.
void JumpEnded(void* payload) noexcept
{
    const auto original = reinterpret_cast<JumpEndedFn>(g_jumpEnded.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(payload, kJumpWeak)))
    {
        original(payload);
        return;
    }
    NoteSource(g_counts.jumpTwo);
    const OwnerTwo  owner;
    const JumpRoute route;
    original(payload);
}

/// The drift action's state 1 tick (anim worker): player 2's chocobo's drift is player 2's, and the game's gate (is the
/// chocobo's rider the player?) answers yes for player 2's member on this thread.
std::uint64_t Drift(void* action, double dt) noexcept
{
    const auto original = reinterpret_cast<DriftFn>(g_driftSlot.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(action, kDriftChocobo)))
    {
        return original(action, dt);
    }
    NoteSource(g_counts.driftTwo);
    NoteAction();
    const OwnerTwo                               owner;
    const member_movement::ThreadLeaderAnswer leader(g_memberType.load(std::memory_order_relaxed));
    return original(action, dt);
}

/// The brake move's phases 1 and 2 (anim worker), as the drift.
std::uint64_t Brake(platform::VTableEntry& slot, void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    const auto original = reinterpret_cast<BrakeFn>(slot.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(move, kMoveCharacter)))
    {
        return original(move, event, xmm2, xmm3);
    }
    NoteSource(g_counts.brakeTwo);
    NoteAction();
    const OwnerTwo                               owner;
    const member_movement::ThreadLeaderAnswer leader(g_memberType.load(std::memory_order_relaxed));
    return original(move, event, xmm2, xmm3);
}

std::uint64_t BrakeOne(void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    return Brake(g_brakeOneSlot, move, event, xmm2, xmm3);
}

std::uint64_t BrakeTwo(void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    return Brake(g_brakeTwoSlot, move, event, xmm2, xmm3);
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
        g_copiedCount = 0;  // the shared ride's copies go with the list
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
    const auto scooter = g_counts.scooterTwo.exchange(0, std::memory_order_relaxed);
    const auto glide   = g_counts.glideTwo.exchange(0, std::memory_order_relaxed);
    const auto jump    = g_counts.jumpTwo.exchange(0, std::memory_order_relaxed);
    const auto drift   = g_counts.driftTwo.exchange(0, std::memory_order_relaxed);
    const auto brake   = g_counts.brakeTwo.exchange(0, std::memory_order_relaxed);
    const auto oa      = g_counts.ownerAddsTwo.exchange(0, std::memory_order_relaxed);
    const auto os      = g_counts.ownerStopsTwo.exchange(0, std::memory_order_relaxed);
    const auto left    = g_counts.ownerStopsLeft.exchange(0, std::memory_order_relaxed);
    const auto worker  = g_counts.sourceWorker.exchange(0, std::memory_order_relaxed);
    const auto onGame  = g_counts.actionsOnGame.exchange(0, std::memory_order_relaxed);
    if (scooter + glide + jump + drift + brake != 0)
    {
        platform::LogDebug(L"rumble list ({} s): player 2's sources outside player 2's scopes: scooter {}, glide {}, jump "
                           L"box {}, drift {}, brake {} ({} off the game thread; drift and brake on it {}); their adds "
                           L"to player 2's list {}, stops on it {}, worker stops left {}",
                           kSummaryEvery.count(), scooter, glide, jump, drift, brake, worker, onGame, oa, os, left);
    }
}

/// The 1.004 jump box's list load, then: on the thread JumpEnded named (g_jumpThread, against the TEB's thread id at
/// gs:[48h]), player 2's list when it exists. It runs under the game's lock, which DropList also takes to clear
/// g_listTwo, so the list it takes is live for the whole add. No call, no stack use; rax, rcx and the flags are dead at
/// that point (overwritten before any read: 0x1422AD988, 0x1422AD997, 0x1422AD99B). The code reads this DLL's globals,
/// which stay mapped: the DLL is pinned (platform/module_pin.h).
bool PlaceJumpListLoad(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() != platform::BuildIndex::Research)
    {
        return false;
    }
    const auto site = image.Rebase(kJumpListLoad, sizeof(kJumpListLoadBytes));
    if (!site)
    {
        return false;
    }
    const auto thread = reinterpret_cast<std::uint64_t>(&g_jumpThread);
    const auto list   = reinterpret_cast<std::uint64_t>(&g_listTwo);
    std::array<std::uint8_t, 49> code{
        0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00,        // mov rsi,[rsi+3B0h]  (the game's load)
        0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,  // mov eax,gs:[48h]    (this thread's id)
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rcx,&g_jumpThread
        0x3B, 0x01,                                      // cmp eax,[rcx]
        0x75, 0x14,                                      // jne done
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rcx,&g_listTwo
        0x48, 0x8B, 0x09,                                // mov rcx,[rcx]
        0x48, 0x85, 0xC9,                                // test rcx,rcx
        0x48, 0x0F, 0x45, 0xF1,                          // cmovnz rsi,rcx
    };                                                   // done: (the jump back follows)
    std::memcpy(code.data() + 17, &thread, sizeof(thread));
    std::memcpy(code.data() + 31, &list, sizeof(list));
    return g_jumpListLoad.Install(*site, kJumpListLoadBytes, code, *site + sizeof(kJumpListLoadBytes));
}

/// A slot of a source, checked against the function the research read. Logged by the caller.
bool Slot(const platform::GameImage& image, platform::VTableEntry& entry, std::uintptr_t slot, std::uintptr_t function,
          std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(slot, sizeof(void*));
    const auto fn = image.Rebase(function);
    return at && fn && entry.Replace(*at, *fn, replacement);
}

/// Player 2's rumble sources outside player 2's scopes (each optional). Returns the install line's text.
std::wstring InstallSources(const platform::GameImage& image) noexcept
{
    const bool scooter =
        Slot(image, g_scooterTickSlot, kScooterTickSlot, kScooterTick, reinterpret_cast<std::uintptr_t>(&ScooterTick)) &&
        Slot(image, g_scooterOffSlot, kScooterOffSlot, kScooterOff, reinterpret_cast<std::uintptr_t>(&ScooterOff));
    if (!scooter && g_scooterTickSlot.Original() != 0)
    {
        g_scooterTickSlot.Restore();  // both or neither: the loop added in slot 1 is stopped by slot 8 at getoff
    }
    const bool glide =
        Slot(image, g_glideSafeSlot, kGlideSafeSlot, kGlideSafe, reinterpret_cast<std::uintptr_t>(&GlideSafe)) &&
        Slot(image, g_glideRunSlot, kGlideRunSlot, kGlideRun, reinterpret_cast<std::uintptr_t>(&GlideRun));
    if (!glide && g_glideSafeSlot.Original() != 0)
    {
        g_glideSafeSlot.Restore();  // both or neither: Broadcast calls one, a direct call the other
    }
    // 1.004's add is inline: its list load must follow the invoker, so the invoker waits for it there.
    const bool research = platform::RunningBuild() == platform::BuildIndex::Research;
    const bool load     = research && PlaceJumpListLoad(image);
    const auto jumpAt   = image.Rebase(kJumpEnded);
    const bool jump     = (load || !research) && jumpAt && image.BytesMatch(kJumpEnded, kJumpEndedBytes) &&
                      g_jumpEnded.Install(*jumpAt, image.EntryInstruction(kJumpEnded, kJumpEndedBytes),
                                          reinterpret_cast<std::uintptr_t>(&JumpEnded));
    if (!jump && g_jumpListLoad.IsInstalled())
    {
        g_jumpListLoad.Restore();
    }
    const bool drift = Slot(image, g_driftSlot, kDriftSlot, kDrift, reinterpret_cast<std::uintptr_t>(&Drift));
    const bool brake =
        Slot(image, g_brakeOneSlot, kBrakeOneSlot, kBrakeOne, reinterpret_cast<std::uintptr_t>(&BrakeOne)) &&
        Slot(image, g_brakeTwoSlot, kBrakeTwoSlot, kBrakeTwo, reinterpret_cast<std::uintptr_t>(&BrakeTwo));
    if (!brake && g_brakeOneSlot.Original() != 0)
    {
        g_brakeOneSlot.Restore();  // both phases or neither
    }
    auto part = [](bool on) { return on ? L"on" : L"OFF (not what the research read)"; };
    try
    {
        return std::format(L"player 2's sources outside their scopes: scooter {}, glide {}, jump box {}{}, drift {}, "
                           L"brake {}",
                           part(scooter), part(glide), part(jump),
                           research && jump ? L" (1.004: its inline add's list load)" : L"", part(drift), part(brake));
    }
    catch (...)
    {
        return {};  // formatting fails only out of memory: the install line then goes without this part
    }
}

void RemoveSources() noexcept
{
    for (platform::VTableEntry* slot : {&g_brakeTwoSlot, &g_brakeOneSlot, &g_driftSlot, &g_glideRunSlot,
                                        &g_glideSafeSlot, &g_scooterOffSlot, &g_scooterTickSlot})
    {
        if (slot->Original() != 0)
        {
            slot->Restore();
        }
    }
    if (g_jumpEnded.IsInstalled())
    {
        g_jumpEnded.Restore();
    }
    if (g_jumpListLoad.IsInstalled())
    {
        g_jumpListLoad.Restore();
    }
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
    // The sources outside player 2's scopes (each optional: without one, its rumble plays on pad 1 as before).
    const std::wstring sources = g_installed ? InstallSources(image) : std::wstring{};
    platform::Log(L"rumble list: {}{}{}{}",
                  g_installed ? L"each player's traversal rumble (zip line, chocobo, ropes, gimmicks) plays and stops on "
                                L"their own pad"
                              : L"UNCHANGED, the game's one list plays on the primary player's pad (a function or call "
                                L"is not what the research read)",
                  !g_installed ? L""
                  : feet       ? L"; player 2's chocobo's footsteps run in player 2's scope"
                               : L"; the chocobo's footstep slot is not what the research read: player 2's footsteps stay "
                                 L"on the primary player's pad",
                  sources.empty() ? L"" : L"; ", sources);
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
    RemoveSources();  // before the add and the stop they route through
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
        g_chocoboWeak.store(0, std::memory_order_relaxed);
        g_memberType.store(-1, std::memory_order_relaxed);
        g_copyOne.store(false, std::memory_order_release);
        DropList(L"player 2 is out");
        return;
    }
    const void* const chocobo = chocobo_ride::DrivenChocobo();
    g_footChocobo.store(chocobo, std::memory_order_relaxed);
    g_chocoboWeak.store(chocobo != nullptr ? leader_scope::WeakOf(const_cast<void*>(chocobo)) : 0,
                        std::memory_order_relaxed);
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
    const auto type = member != nullptr ? party_member::PlayerTypeOf(member) : std::nullopt;
    g_memberType.store(type ? static_cast<int>(*type) : -1, std::memory_order_relaxed);
    if (g_listTwo.load(std::memory_order_relaxed) == nullptr && generation::PlayerTwoController() != nullptr)
    {
        MakeList();
    }
    LogSummary();
}

void CopyPlayerOne(bool on) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    on = on && g_listTwo.load(std::memory_order_acquire) != nullptr;
    if (g_copyOne.load(std::memory_order_relaxed) == on)
    {
        return;
    }
    if (on)
    {
        std::array<std::uint64_t, 16> seeded{};
        const std::size_t             count = SeedTwo(seeded);
        try
        {
            std::wstring names;
            for (std::size_t i = 0; i < count; ++i)
            {
                names += (i == 0 ? L"" : L", ") + KeyText(seeded[i]);
            }
            platform::Log(L"rumble list: shared ride: player 1's rumble list is copied into player 2's (both pads); "
                          L"loops already playing, added to player 2's now: {}",
                          names.empty() ? L"none" : names);
        }
        catch (...)
        {
        }
        return;
    }
    g_copyOne.store(false, std::memory_order_release);
    // The ride is over: what was copied and not stopped stops on player 2's list now, as the game stops it on player
    // 1's.
    std::array<std::uint64_t, 16> left{};
    std::size_t                   count = 0;
    {
        const ListLock lock;
        left          = g_copiedKeys;
        count         = g_copiedCount;
        g_copiedCount = 0;
    }
    const auto original = reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
    for (std::size_t i = 0; i < count; ++i)
    {
        StopTwo(original, left[i]);
    }
    try
    {
        std::wstring names;
        for (std::size_t i = 0; i < count; ++i)
        {
            names += (i == 0 ? L"" : L", ") + KeyText(left[i]);
        }
        platform::Log(L"rumble list: shared ride over: copies {}, their stops {} (on a worker, not copied {}); stopped "
                      L"now on player 2's list: {}",
                      g_counts.copiesTwo.exchange(0, std::memory_order_relaxed),
                      g_counts.copyStopsTwo.exchange(0, std::memory_order_relaxed),
                      g_counts.copyStopsLeft.exchange(0, std::memory_order_relaxed), names.empty() ? L"none" : names);
    }
    catch (...)
    {
    }
}

void PlayerTwoOut() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        CopyPlayerOne(false);
        g_memberWeak.store(0, std::memory_order_relaxed);
        g_chocoboWeak.store(0, std::memory_order_relaxed);
        g_memberType.store(-1, std::memory_order_relaxed);
        DropList(L"player 2 leaves");
    }
}

}  // namespace lss::player_two::rumble_list_two
