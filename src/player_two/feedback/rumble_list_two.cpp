#include "player_two/feedback/rumble_list_two.h"

#include "facts/engine.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/feedback/rumble_list_shared.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/session/party_member.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// The rumble list's core (player_two/feedback/rumble_list_two.h): the add and the stop by name, the component's list
// tick,
// the zip line's gate and add, player 2's list's life, the chocobo's footsteps, the summary, and the module's
// install, removal and tick. The shared-ride copy and the sources outside player 2's scopes are their own units.

namespace lss::player_two::rumble_list_two {

namespace {

// 1.004 addresses; 1.005 through replaced-rumble-list.json (every function, call and the lock read from both exes).
constexpr std::uint8_t   kPlayNamedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8],rbx (crosses a block in 1.004)
                                              0x48, 0x89, 0x6C, 0x24, 0x10};  // mov [rsp+10h],rbp
// VibrationList3B0_PlayNamed(key, params*, flag)
constexpr platform::FunctionFact kPlayNamed{L"VibrationList3B0_PlayNamed", 0x141302F0C, kPlayNamedBytes};
constexpr std::size_t    kFirstInstruction  = 5;
constexpr std::size_t    kSecondInstruction = 5;
constexpr std::uintptr_t kAddOrUpdate   = 0x141302E2C;  // VibrationList3B0_AddOrUpdate(list, key, params*, flag)
constexpr std::uintptr_t kListTick      = 0x14125D324;  // Vibration_TickGlobalList_ApplyToPrimaryPC(dt xmm0)
constexpr std::uintptr_t kBuildSet      = 0x14125CFD0;  // Vibration_TickList_BuildChannelSet(list, dt xmm1, set*) -> changed
constexpr std::uintptr_t kApply         = 0x1408A1688;  // PrimaryPC_ApplyVibrationChannels758(list, set*): on PM+0x140
constexpr std::uint8_t   kCtorBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
// VibrationList_Ctor (6 channel slots in 1.004, 7 in 1.005)
constexpr platform::FunctionFact kListCtor{L"VibrationList_Ctor", 0x143727368, kCtorBytes};
constexpr std::uintptr_t kListLock      = 0x14881C428;  // the game's CRITICAL_SECTION around adds and the set build
constexpr std::uint8_t   kRowsBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08};
// LeaderActionComp_PerFrameB_RowsAndVibration(comp, xmm1, xmm2)
constexpr platform::FunctionFact kRows{L"LeaderActionComp_PerFrameB_RowsAndVibration", 0x14125D474, kRowsBytes};
constexpr std::uintptr_t kListTickCall  = 0x14125D558;  // ... its call of the list tick
constexpr std::uintptr_t kZipGateCall   = 0x1433251E2;  // Zipline phase 3: the rumble block's player test (rcx = move)
constexpr std::uintptr_t kZipGate       = 0x141635DE8;  // FieldActionMove_IsPlayerChar_Ex_UCPCOrVehicleOrLeaderTag
constexpr std::uintptr_t kZipAddCall    = 0x1433252C6;  // ... the keep-alive add (call PlayNamed)
// RE-p2-rumble-owner-chocobo.md (read from both exes, 2026-10-12).
constexpr std::uint8_t   kStopNamedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8],rbx
                                              0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
// VibrationList3B0_StopNamed(key): flags the global list's entries
constexpr platform::FunctionFact kStopNamed{L"VibrationList3B0_StopNamed", 0x1422C4818, kStopNamedBytes};
constexpr std::size_t    kStopFirstInstruction = 5;
constexpr std::uintptr_t kListOwner     = 0x14939BC90;  // the list's owner (StopNamed and PlayNamed read it)
constexpr std::uintptr_t kFootSlot      = 0x1457F3ED0;  // EndChocoboCharacter vt+0x978 (1.005 +0x980): footsteps
constexpr std::uintptr_t kFootImpact    = 0x1418BA510;  // ... its function: GetMoveState, then FFB_Chocobo_1/2

// The list object's size, as the global owner's creator allocates it (1.004 `mov ecx,178h` at 0x1436F4AF2, 1.005
// `mov ecx,1B0h` at 0x143429CAA; read from both exes, 2026-10-11).
constexpr platform::PerBuild<std::size_t> kListSize{0x178, 0x1B0};

constexpr std::uint32_t kComponentOwner = 0xA0;   // the leader-action component's controller
constexpr auto          kSummaryEvery   = std::chrono::seconds(10);

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
using FootFn        = double (*)(void* chocobo);  // returns xmm0 as the game's does

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
BuildSetFn              g_buildSet    = nullptr;
ApplyFn                 g_apply       = nullptr;
FreeFn                  g_free        = nullptr;
CtorFn                  g_ctor        = nullptr;
MallocFn                g_malloc      = nullptr;

thread_local const void* t_tickOwner = nullptr;  // the controller whose component's rows tick runs on this thread
thread_local bool        t_zipTwo    = false;    // the zip line's rumble block runs for player 2's member

// Game thread.
std::atomic<std::uint64_t>            g_lastKeyTwo{0};    // the last effect key added for player 2 (Debug on change)
std::uint64_t                         g_lastStopTwo = 0;  // ... and stopped for player 2
platform::Every g_summary{kSummaryEvery};

}  // namespace

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

StopNamedFn StopNamedOriginal() noexcept
{
    return reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
}

namespace {

/// The add by name: player 2's add goes to player 2's list. It is player 2's when its source names player 2 (OwnerTwo,
/// any thread), or on the game thread while player 2 is the primary player (player 2's scopes). During a shared ride
/// an add to the game's list is copied into player 2's list too.
void PlayNamed(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const auto original = reinterpret_cast<PlayNamedFn>(g_playNamed.Original());
    const bool owner    = t_ownerTwo;
    const bool two      = owner || (generation::OnGameThread() && generation::Active() &&
                                    primary_controller::Is(generation::PlayerTwoController()));
    const bool added    = two && AddTwo(key, params, flag);
    const auto route    = rumble_rules::AddRouteOf(two, added, g_copyOne.load(std::memory_order_acquire));
    if (route != rumble_rules::Route::Two)
    {
        g_counts.addsOne.fetch_add(1, std::memory_order_relaxed);
        original(key, params, flag);
        if (route == rumble_rules::Route::GameAndCopy)
        {
            copy::CopyTwo(key, params, flag);
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
    void* const two     = generation::PlayerTwoController();
    switch (rumble_rules::ListTickChoice(list != nullptr, t_tickOwner != nullptr && t_tickOwner == two,
                                         primary_controller::Is(two)))
    {
        case rumble_rules::ListTick::Game: original(dt); return;
        case rumble_rules::ListTick::Two: TickTwo(list, dt); return;
        case rumble_rules::ListTick::Skip:
            g_counts.skippedOne.fetch_add(1, std::memory_order_relaxed);  // the game's list never plays on player 2's pad
            return;
        case rumble_rules::ListTick::GameOne: break;
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
    const std::uint64_t member   = generation::MemberWeak();
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

/// The game's stop by name: player 2's stop goes to player 2's list, by the same rule as the add (game thread, player 2
/// the primary player). Player 2's looping effects (jet, scooter, swimming) were added to player 2's list and never
/// stopped there. During a shared ride a stop on the game's list is copied onto player 2's list too.
void StopNamed(std::uint64_t key) noexcept
{
    const auto original = reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
    const bool owner    = t_ownerTwo;
    const bool game     = generation::OnGameThread();
    const bool hasList  = g_listTwo.load(std::memory_order_acquire) != nullptr;
    const bool two      = owner || (game && generation::Active() && primary_controller::Is(generation::PlayerTwoController()));
    const bool stopped  = two && game && StopTwo(original, key);
    const auto route    = rumble_rules::StopRouteOf(owner, game, hasList, two, stopped, g_copyOne.load(std::memory_order_acquire));
    if (route == rumble_rules::Route::LeftOnWorker)
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
    if (route != rumble_rules::Route::Two)
    {
        g_counts.stopsOne.fetch_add(1, std::memory_order_relaxed);
        original(key);
        if (route == rumble_rules::Route::GameAndCopy)
        {
            copy::CopyStopTwo(original, key, game);
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

struct FootCall
{
    FootFn fn;
    void*  chocobo;
    double result = 0.0;
    bool   added  = false;  // the footstep went to player 2's list
};

void RunFoot(void* context) noexcept
{
    auto&      call   = *static_cast<FootCall*>(context);
    const auto before = g_counts.addsTwo.load(std::memory_order_relaxed);
    call.result       = call.fn(call.chocobo);
    call.added        = g_counts.addsTwo.load(std::memory_order_relaxed) != before;
}

/// The chocobo's footstep slot (every chocobo's; game thread). The footsteps of the chocobo player 2 rides run inside
/// player 2's primary scope, as the game runs the chocobo player 1 rides, so the game's own gate asks player 2's
/// coordinator and the add goes to player 2's list. Every other chocobo's passes through.
double FootImpact(void* chocobo) noexcept
{
    const auto original = reinterpret_cast<FootFn>(g_footSlot.Original());
    const bool twos     = chocobo != nullptr && chocobo == generation::Chocobo();
    if (!generation::OnGameThread())
    {
        // Logging only: the game's gate returns at once on a worker.
        (twos ? g_counts.footWorkerTwo : g_counts.footWorkerOther).fetch_add(1, std::memory_order_relaxed);
        return original(chocobo);
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
        g_copiedKeys.Clear();  // the shared ride's copies go with the list
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
    if (!g_summary.Due(now))
    {
        return;
    }
    const auto a1   = platform::Take(g_counts.addsOne);
    const auto a2   = platform::Take(g_counts.addsTwo);
    const auto z2   = platform::Take(g_counts.zipAddsTwo);
    const auto z1   = platform::Take(g_counts.zipAddsOne);
    const auto t1   = platform::Take(g_counts.ticksOne);
    const auto t2   = platform::Take(g_counts.ticksTwo);
    const auto p2   = platform::Take(g_counts.appliedTwo);
    const auto skip = platform::Take(g_counts.skippedOne);
    const auto ref  = platform::Take(g_counts.refused);
    const auto s1   = platform::Take(g_counts.stopsOne);
    const auto s2   = platform::Take(g_counts.stopsTwo);
    const auto foot = platform::Take(g_counts.footTwo);
    const auto footRefused     = platform::Take(g_counts.footRefused);
    const auto footWorkerTwo   = platform::Take(g_counts.footWorkerTwo);
    const auto footOther       = platform::Take(g_counts.footOther);
    const auto footWorkerOther = platform::Take(g_counts.footWorkerOther);
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
    const auto scooter = platform::Take(g_counts.scooterTwo);
    const auto glide   = platform::Take(g_counts.glideTwo);
    const auto jump    = platform::Take(g_counts.jumpTwo);
    const auto drift   = platform::Take(g_counts.driftTwo);
    const auto brake   = platform::Take(g_counts.brakeTwo);
    const auto oa      = platform::Take(g_counts.ownerAddsTwo);
    const auto os      = platform::Take(g_counts.ownerStopsTwo);
    const auto left    = platform::Take(g_counts.ownerStopsLeft);
    const auto worker  = platform::Take(g_counts.sourceWorker);
    const auto onGame  = platform::Take(g_counts.actionsOnGame);
    if (scooter + glide + jump + drift + brake != 0)
    {
        platform::LogDebug(L"rumble list ({} s): player 2's sources outside player 2's scopes: scooter {}, glide {}, jump "
                           L"box {}, drift {}, brake {} ({} off the game thread; drift and brake on it {}); their adds "
                           L"to player 2's list {}, stops on it {}, worker stops left {}",
                           kSummaryEvery.count(), scooter, glide, jump, drift, brake, worker, onGame, oa, os, left);
    }
}

template <class Fn>
bool Find(const platform::GameImage& image, std::uintptr_t address, Fn& out) noexcept
{
    const auto at = image.Rebase(address);
    out           = at ? reinterpret_cast<Fn>(*at) : nullptr;
    return out != nullptr;
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto at = platform::FindFunction(image, fact);
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
                           Find(image, facts::kFree, g_free) && Find(image, kListCtor, g_ctor) &&
                           Find(image, facts::kMalloc, g_malloc);
    // The add's entry: its first instruction crosses a block in 1.004, so the second is replaced there.
    bool       add    = false;
    const auto bytes  = kPlayNamed.entryBytes;
    const auto playAt = platform::FindFunction(image, kPlayNamed);
    if (functions && playAt)
    {
        const auto replacement = reinterpret_cast<std::uintptr_t>(&PlayNamed);
        add = g_playNamed.Install(*playAt, image.EntryInstruction(kPlayNamed.address, bytes.first(kFirstInstruction)),
                                  replacement) ||
              g_playNamed.InstallAtSecondInstruction(*playAt, bytes.first(kFirstInstruction),
                                                     bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
    }
    const auto rowsAt = platform::FindFunction(image, kRows);
    const bool rows   = add && rowsAt &&
                      g_rows.Install(*rowsAt, image.EntryInstruction(kRows.address, kRows.entryBytes),
                                     reinterpret_cast<std::uintptr_t>(&Rows));
    const bool tick = rows && g_listTickCall.InstallResearch(image, kListTickCall, kListTick,
                                                             reinterpret_cast<std::uintptr_t>(&ListTick));
    const bool zip  = tick &&
                     g_zipAddCall.InstallResearch(image, kZipAddCall, kPlayNamed.address,
                                                  reinterpret_cast<std::uintptr_t>(&ZipAdd)) &&
                     g_zipGateCall.InstallResearch(image, kZipGateCall, kZipGate,
                                                   reinterpret_cast<std::uintptr_t>(&ZipGate));
    // The stop by name: player 2's loops added to player 2's list must stop there.
    const auto owner  = image.Rebase(kListOwner, sizeof(void*));
    g_listOwner       = owner ? *owner : 0;
    const auto stopAt = platform::FindFunction(image, kStopNamed);
    const bool stop   = zip && g_listOwner != 0 && stopAt &&
                      g_stopNamed.Install(*stopAt,
                                          image.EntryInstruction(kStopNamed.address,
                                                                 kStopNamed.entryBytes.first(kStopFirstInstruction)),
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
    // The sources outside player 2's scopes (each optional: without one, its rumble plays on pad 1 as before).
    const std::wstring sources = g_installed ? sources::Install(image) : std::wstring{};
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
    sources::Remove();  // before the add and the stop they route through
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
        g_memberType.store(-1, std::memory_order_relaxed);
        g_copyOne.store(false, std::memory_order_release);
        DropList(L"player 2 is out");
        return;
    }
    void* const member = generation::Member();
    const auto type = member != nullptr ? party_member::PlayerTypeOf(member) : std::nullopt;
    g_memberType.store(type ? static_cast<int>(*type) : -1, std::memory_order_relaxed);
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
        CopyPlayerOne(false);
        g_memberType.store(-1, std::memory_order_relaxed);
        DropList(L"player 2 leaves");
    }
}

}  // namespace lss::player_two::rumble_list_two
