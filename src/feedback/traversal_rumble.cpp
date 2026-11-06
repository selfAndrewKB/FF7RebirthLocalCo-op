#include "feedback/traversal_rumble.h"

#include "facts/engine.h"
#include "feedback/traversal_rumble_shared.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "rides/chocobo_ride.h"
#include "scopes/primary_controller.h"
#include "session/party_member.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

// The rumble list's core (feedback/traversal_rumble.h): the add and the stop by name, the component's list tick, the
// zip line's gate and add, each seat's list's life, the chocobo's footsteps, the summary, and the module's install,
// removal and tick. The shared-ride copy and the sources outside the seats' scopes are their own units. The seat of an
// add or a stop is its source's (OwnerTwo) or the primary player's (traversal_rumble_rules::SeatOfAdd); of a list tick,
// its component's controller's; of the zip line, its move's character; of a footstep, the chocobo the seat rides.

namespace lss::feedback::traversal_rumble {

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
thread_local int         t_zipSeat   = 0;        // the zip line's rumble block runs for this seat's member (0: none)

// By seat (index 0 unused): the last effect key added for the seat (Debug on change), and stopped for it (game thread).
std::atomic<std::uint64_t> g_lastKeyTwo[players::kMaxPlayers]{};
std::uint64_t              g_lastStopTwo[players::kMaxPlayers]{};
platform::Every            g_summary{kSummaryEvery};

/// The built active seat that is the primary player now, or 0 (player 1's, or none). Game thread.
int PrimarySeat() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && scopes::primary_controller::Is(players::generation::ControllerOf(seat)))
        {
            return seat;
        }
    }
    return 0;
}

/// The built seat that drives `chocobo` this tick (generation's), or 0. Any thread.
int SeatOfChocobo(const void* chocobo) noexcept
{
    for (int seat = 1; chocobo != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::ChocoboOf(seat) == chocobo)
        {
            return seat;
        }
    }
    return 0;
}

/// The reasons a seat's list drop logs at its leave (literals, by seat).
const wchar_t* OutText(int seat) noexcept
{
    return seat == players::kFirstFurtherSeat ? L"player 2 is out" : L"player 3 is out";
}
const wchar_t* LeavesText(int seat) noexcept
{
    return seat == players::kFirstFurtherSeat ? L"player 2 leaves" : L"player 3 leaves";
}

}  // namespace

/// `seat`'s add, as the game's add does it, into the seat's list. False if the seat has no list now.
bool AddTwo(int seat, std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    if (!players::SeatIndex(seat))
    {
        return false;
    }
    const ListLock lock;
    void* const    list = g_listTwo[seat].load(std::memory_order_acquire);
    if (list == nullptr)
    {
        return false;
    }
    g_addOrUpdate(list, key, params, flag);
    return true;
}

/// `seat`'s stop, as the game's stop does it, on the seat's list: under the game's lock (re-entrant, so the game's own
/// lock inside nests), the owner names the seat's list for the length of the call. Every other reader of the owner's
/// list is under that lock or on this thread (RE-p2-zipline-rumble-hook-who.md §4.2), so no one else sees the swap; one
/// seat's swap is put back before the next seat's begins. False if the seat has no list now. Game thread.
bool StopTwo(int seat, StopNamedFn original, std::uint64_t key) noexcept
{
    if (!players::SeatIndex(seat))
    {
        return false;
    }
    const ListLock lock;
    void* const    list  = g_listTwo[seat].load(std::memory_order_acquire);
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
        platform::Log(L"WARNING: rumble list: the game's stop faulted on {}'s list ({}); the game's list is "
                      L"restored",
                      players::SeatName(seat), platform::DescribeFault(platform::LastFault()).text);
    }
    return true;
}

StopNamedFn StopNamedOriginal() noexcept
{
    return reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
}

namespace {

/// The add by name: a seat's add goes to the seat's list. It is a seat's when its source names the seat (OwnerTwo, any
/// thread), or on the game thread while the seat is the primary player (the seat's scopes). During a shared ride an
/// add to the game's list is copied into each copying seat's list too.
void PlayNamed(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const auto original = reinterpret_cast<PlayNamedFn>(g_playNamed.Original());
    const int  owner    = t_ownerSeat;
    const bool game     = owner == 0 && players::generation::OnGameThread();
    const int  seat     = traversal_rumble_rules::SeatOfAdd(owner, game, game ? PrimarySeat() : 0);
    const bool two      = seat >= 1;
    const bool added    = two && AddTwo(seat, key, params, flag);
    const auto route    = traversal_rumble_rules::AddRouteOf(two, added, AnyCopy());
    if (route != traversal_rumble_rules::Route::Two)
    {
        g_counts[0].addsOne.fetch_add(1, std::memory_order_relaxed);
        original(key, params, flag);
        if (route == traversal_rumble_rules::Route::GameAndCopy)
        {
            copy::CopyTwo(key, params, flag);
        }
        return;
    }
    Counts&              counts = g_counts[seat];
    const wchar_t* const name   = players::SeatName(seat);
    (owner != 0 ? counts.ownerAddsTwo : counts.addsTwo).fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} added to {}'s list", KeyText(key), name);
    if (g_lastKeyTwo[seat].exchange(key, std::memory_order_relaxed) != key && platform::Enabled(platform::Level::Debug))
    {
        // The seat's name as its own argument: nothing is built for a line that is off.
        const std::uint64_t effect = params != nullptr ? ReadField<std::uint64_t>(params, 0) : 0;
        if (owner != 0)
        {
            platform::LogDebug(L"rumble list: {} ({}) goes to {}'s list (its source is {}'s)", KeyText(key),
                               KeyText(effect), name, name);
        }
        else
        {
            platform::LogDebug(L"rumble list: {} ({}) goes to {}'s list ({} is the primary player)", KeyText(key),
                               KeyText(effect), name, name);
        }
    }
}

/// Plays `seat`'s list on the seat's controller, as the game's list tick plays the global one (game thread).
void TickTwo(int seat, void* list, float dt) noexcept
{
    Counts&    counts = g_counts[seat];
    ChannelSet set;
    bool       changed = false;
    {
        const ListLock lock;
        changed = g_buildSet(list, dt, &set);
    }
    counts.ticksTwo.fetch_add(1, std::memory_order_relaxed);
    if (changed)
    {
        const scopes::primary_controller::Scope primary(seat);  // already held inside the seat's scopes
        if (primary.Held())
        {
            g_apply(list, &set);
            counts.appliedTwo.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            counts.refused.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (set.data != nullptr)
    {
        g_free(set.data);
    }
}

/// The component's list tick: a seat's component ticks the seat's list, player 1's the game's.
void ListTick(float dt) noexcept
{
    const auto original = reinterpret_cast<ListTickFn>(g_listTickCall.Original());
    const bool game     = players::generation::OnGameThread();
    // The component's seat (by its controller) and the active seats' lists, on the game thread only.
    int ownerSeat = game && t_tickOwner != nullptr ? players::generation::SeatOfController(t_tickOwner) : -1;
    ownerSeat     = ownerSeat >= 1 && players::generation::Active(ownerSeat) ? ownerSeat : -1;
    void* const ownerList = ownerSeat >= 1 ? g_listTwo[ownerSeat].load(std::memory_order_acquire) : nullptr;
    bool        anyList   = false;
    for (int seat = 1; game && seat <= players::kSeatsBuilt; ++seat)
    {
        anyList = anyList || (players::generation::Active(seat) && g_listTwo[seat].load(std::memory_order_acquire) != nullptr);
    }
    const int primary = game ? PrimarySeat() : 0;
    switch (traversal_rumble_rules::ListTickFor(ownerSeat, ownerList != nullptr, anyList, primary >= 1))
    {
        case traversal_rumble_rules::ListTick::Game: original(dt); return;
        case traversal_rumble_rules::ListTick::Two: TickTwo(ownerSeat, ownerList, dt); return;
        case traversal_rumble_rules::ListTick::Skip:
            // The game's list never plays on a seat's pad.
            g_counts[primary].skippedOne.fetch_add(1, std::memory_order_relaxed);
            return;
        case traversal_rumble_rules::ListTick::GameOne: break;
    }
    g_counts[0].ticksOne.fetch_add(1, std::memory_order_relaxed);
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

/// The zip line's rumble test (anim worker): yes for a seat's member (the seat from the move's character), as for
/// player 1's character; the add that follows goes to the seat's list.
bool ZipGate(void* move) noexcept
{
    const auto original = reinterpret_cast<ZipGateFn>(g_zipGateCall.Original());
    t_zipSeat           = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt && t_zipSeat == 0; ++seat)
    {
        const std::uint64_t member = players::generation::MemberWeakOf(seat);
        t_zipSeat = member != 0 && ReadField<std::uint64_t>(move, kMoveCharacter) == member &&
                            g_listTwo[seat].load(std::memory_order_acquire) != nullptr
                        ? seat
                        : 0;
    }
    return t_zipSeat != 0 || original(move);
}

void ZipAdd(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const int seat = t_zipSeat;
    t_zipSeat      = 0;
    if (seat >= 1 && AddTwo(seat, key, params, flag))
    {
        g_counts[seat].zipAddsTwo.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_counts[0].zipAddsOne.fetch_add(1, std::memory_order_relaxed);
    reinterpret_cast<PlayNamedFn>(g_zipAddCall.Original())(key, params, flag);
}

/// The game's stop by name: a seat's stop goes to the seat's list, by the same rule as the add (its source's seat, or
/// on the game thread the seat that is the primary player). A seat's looping effects (jet, scooter, swimming) were
/// added to its list and never stopped there. During a shared ride a stop on the game's list is copied onto each
/// copying seat's list too.
void StopNamed(std::uint64_t key) noexcept
{
    const auto original = reinterpret_cast<StopNamedFn>(g_stopNamed.Original());
    const int  owner    = t_ownerSeat;
    const bool game     = players::generation::OnGameThread();
    const int  seat     = traversal_rumble_rules::SeatOfAdd(owner, game, owner == 0 && game ? PrimarySeat() : 0);
    const bool two      = seat >= 1;
    const bool hasList  = two && g_listTwo[seat].load(std::memory_order_acquire) != nullptr;
    const bool stopped  = two && game && StopTwo(seat, original, key);
    const auto route    = traversal_rumble_rules::StopRouteOf(owner != 0, game, hasList, two, stopped, AnyCopy());
    if (route == traversal_rumble_rules::Route::LeftOnWorker)
    {
        // A source of a seat's stopping on a worker: the owner's list can be named only on the game thread, and no
        // source found stops there (RE-p2-rumble-owner-chocobo.md §9.4). Player 1's entries are left alone.
        if (g_counts[seat].ownerStopsLeft.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            platform::Log(L"rumble list: a stop of {}'s ({}) ran on a worker; player 1's list was left alone "
                          L"(counted in the Debug summary)",
                          players::SeatName(seat), KeyText(key));
        }
        return;
    }
    if (route != traversal_rumble_rules::Route::Two)
    {
        g_counts[0].stopsOne.fetch_add(1, std::memory_order_relaxed);
        original(key);
        if (route == traversal_rumble_rules::Route::GameAndCopy)
        {
            copy::CopyStopTwo(original, key, game);
        }
        return;
    }
    Counts&              counts = g_counts[seat];
    const wchar_t* const name   = players::SeatName(seat);
    (owner != 0 ? counts.ownerStopsTwo : counts.stopsTwo).fetch_add(1, std::memory_order_relaxed);
    platform::LogTrace(L"rumble list: {} stopped on {}'s list", KeyText(key), name);
    if (key != g_lastStopTwo[seat])
    {
        g_lastStopTwo[seat] = key;
        if (owner != 0)
        {
            platform::LogDebug(L"rumble list: {} stops on {}'s list (its source is {}'s)", KeyText(key), name, name);
        }
        else
        {
            platform::LogDebug(L"rumble list: {} stops on {}'s list ({} is the primary player)", KeyText(key), name,
                               name);
        }
    }
}

struct FootCall
{
    int    seat;  // the seat that rides the chocobo
    FootFn fn;
    void*  chocobo;
    double result = 0.0;
    bool   added  = false;  // the footstep went to the seat's list
};

void RunFoot(void* context) noexcept
{
    auto&      call   = *static_cast<FootCall*>(context);
    const auto before = g_counts[call.seat].addsTwo.load(std::memory_order_relaxed);
    call.result       = call.fn(call.chocobo);
    call.added        = g_counts[call.seat].addsTwo.load(std::memory_order_relaxed) != before;
}

/// The chocobo's footstep slot (every chocobo's; game thread). The footsteps of the chocobo a seat rides run inside the
/// seat's primary scope, as the game runs the chocobo player 1 rides, so the game's own gate asks the seat's
/// coordinator and the add goes to the seat's list. Every other chocobo's passes through.
double FootImpact(void* chocobo) noexcept
{
    const auto original = reinterpret_cast<FootFn>(g_footSlot.Original());
    const int  driver   = SeatOfChocobo(chocobo);  // the seat driving it this tick, or 0
    if (!players::generation::OnGameThread())
    {
        // Logging only: the game's gate returns at once on a worker.
        (driver >= 1 ? g_counts[driver].footWorkerTwo : g_counts[0].footWorkerOther)
            .fetch_add(1, std::memory_order_relaxed);
        return original(chocobo);
    }
    if (driver == 0)
    {
        g_counts[0].footOther.fetch_add(1, std::memory_order_relaxed);
    }
    const int rider = rides::chocobo_ride::SeatRiding(chocobo);  // the seat riding it seated, or -1
    FootCall  call{rider, original, chocobo};
    if (rider >= 1 && g_listTwo[rider].load(std::memory_order_acquire) != nullptr &&
        rides::chocobo_ride::RunAsRider(chocobo, &RunFoot, &call))
    {
        g_counts[rider].footTwo.fetch_add(1, std::memory_order_relaxed);
        platform::LogTrace(L"rumble list: {}'s chocobo's footsteps ran in {}'s scope (added {})",
                           players::SeatName(rider), players::SeatName(rider), call.added ? 1 : 0);
        return call.result;
    }
    if (driver >= 1)
    {
        g_counts[driver].footRefused.fetch_add(1, std::memory_order_relaxed);
        platform::LogTrace(L"rumble list: {}'s chocobo's footstep not run in {}'s scope (ridden {})",
                           players::SeatName(driver), players::SeatName(driver),
                           rides::chocobo_ride::RiddenChocobo(driver) == chocobo ? 1 : 0);
    }
    return original(chocobo);
}

/// Game thread: `seat`'s list, made by the game's allocator and constructor.
void MakeList(int seat) noexcept
{
    void* const list = g_malloc(kListSize);
    if (list != nullptr)
    {
        g_ctor(list);
    }
    if (list == nullptr)
    {
        platform::Log(L"WARNING: rumble list: {}'s list could not be made; the game's list plays on the primary "
                      L"player's pad",
                      players::SeatName(seat));
        return;
    }
    {
        const ListLock lock;
        g_listTwo[seat].store(list, std::memory_order_release);
    }
    g_lastKeyTwo[seat] = 0;
    platform::Log(L"rumble list: {} has their own rumble list (traversal rumble per pad)", players::SeatName(seat));
}

/// Game thread: stops `seat`'s channels (when the seat can still be the primary player) and destroys its list.
void DropList(int seat, const wchar_t* why) noexcept
{
    if (g_listTwo[seat].load(std::memory_order_acquire) == nullptr)
    {
        return;  // only this thread makes or drops the list
    }
    void* list = nullptr;
    {
        const ListLock lock;
        list = g_listTwo[seat].exchange(nullptr, std::memory_order_acq_rel);
        if (list != nullptr)
        {
            WriteField<std::int32_t>(list, kListEntries + kArrayNum, 0);  // as the game's own clear
            // The jump box's routes stop naming it (their load reads a route's list under this lock).
            for (auto& slot : g_jumpSlots)
            {
                void* routed = list;
                slot.list.compare_exchange_strong(routed, nullptr, std::memory_order_acq_rel);
            }
        }
        g_copiedKeys[seat].Clear();  // the shared ride's copies go with the list
    }
    if (list == nullptr)
    {
        return;
    }
    const wchar_t* const name    = players::SeatName(seat);
    bool                 stopped = false;
    if (players::generation::Active(seat))
    {
        const scopes::primary_controller::Scope primary(seat);
        if (primary.Held())
        {
            ChannelSet none;
            g_apply(list, &none);  // every channel of the seat's list stops on the seat's controller
            stopped = true;
        }
    }
    const auto destroy = reinterpret_cast<DeleteFn>(ReadPointer(ReadPointer(list, 0), 0));
    if (destroy != nullptr)
    {
        destroy(list, 1);
    }
    try
    {
        platform::Log(L"rumble list: {}'s rumble list removed ({}; its channels {})", name, why,
                      stopped ? std::format(L"stopped on {}'s controller", name)
                              : std::format(L"not stopped: {} is no longer the primary player", name));
    }
    catch (...)
    {
    }
}

/// The Debug summary: a line per seat with a list, each with the game's list's counts (taken once for all of them).
void LogSummary() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    Counts&    game            = g_counts[0];
    const auto a1              = platform::Take(game.addsOne);
    const auto z1              = platform::Take(game.zipAddsOne);
    const auto t1              = platform::Take(game.ticksOne);
    const auto s1              = platform::Take(game.stopsOne);
    const auto footOther       = platform::Take(game.footOther);
    const auto footWorkerOther = platform::Take(game.footWorkerOther);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Counts&              counts        = g_counts[seat];
        const wchar_t* const name          = players::SeatName(seat);
        const auto           a2            = platform::Take(counts.addsTwo);
        const auto           z2            = platform::Take(counts.zipAddsTwo);
        const auto           t2            = platform::Take(counts.ticksTwo);
        const auto           p2            = platform::Take(counts.appliedTwo);
        const auto           skip          = platform::Take(counts.skippedOne);
        const auto           ref           = platform::Take(counts.refused);
        const auto           s2            = platform::Take(counts.stopsTwo);
        const auto           foot          = platform::Take(counts.footTwo);
        const auto           footRefused   = platform::Take(counts.footRefused);
        const auto           footWorkerTwo = platform::Take(counts.footWorkerTwo);
        if (a1 + a2 + z1 + z2 + t1 + t2 == 0 || g_listTwo[seat].load(std::memory_order_relaxed) == nullptr)
        {
            continue;
        }
        platform::LogDebug(L"rumble list ({} s): adds to the game's list {} (zip line {}), to {}'s {} (zip line {}); "
                           L"stops on the game's list {}, on {}'s {}; {}'s chocobo footsteps in {}'s scope "
                           L"{} (not in it {}, on a worker {}), other chocobos' footsteps {} (on a worker {}); ticks of "
                           L"the game's list {}, of {}'s {} (played on {}'s controller {}, refused {}); the game's "
                           L"list skipped inside {}'s scope {}",
                           kSummaryEvery.count(), a1, z1, name, a2 + z2, z2, s1, name, s2, name, name, foot,
                           footRefused, footWorkerTwo, footOther, footWorkerOther, t1, name, t2, name, p2, ref, name,
                           skip);
        const auto scooter = platform::Take(counts.scooterTwo);
        const auto glide   = platform::Take(counts.glideTwo);
        const auto jump    = platform::Take(counts.jumpTwo);
        const auto drift   = platform::Take(counts.driftTwo);
        const auto brake   = platform::Take(counts.brakeTwo);
        const auto oa      = platform::Take(counts.ownerAddsTwo);
        const auto os      = platform::Take(counts.ownerStopsTwo);
        const auto left    = platform::Take(counts.ownerStopsLeft);
        const auto worker  = platform::Take(counts.sourceWorker);
        const auto onGame  = platform::Take(counts.actionsOnGame);
        if (scooter + glide + jump + drift + brake != 0)
        {
            platform::LogDebug(L"rumble list ({} s): {}'s sources outside {}'s scopes: scooter {}, glide {}, jump "
                               L"box {}, drift {}, brake {} ({} off the game thread; drift and brake on it {}); their "
                               L"adds to {}'s list {}, stops on it {}, worker stops left {}",
                               kSummaryEvery.count(), name, name, scooter, glide, jump, drift, brake, worker, onGame,
                               name, oa, os, left);
        }
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
    // The stop by name: a seat's loops added to its list must stop there.
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
        RemoveRedirects();  // all or none: an add into a seat's list needs the tick that plays it and the stop
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
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // only a built seat makes a list
    {
        if (g_installed && players::generation::OnGameThread())
        {
            DropList(seat, L"the mod is removed");
        }
        else if (g_listTwo[seat].load(std::memory_order_acquire) != nullptr)
        {
            platform::Log(L"WARNING: rumble list: removed off the game thread; {}'s list is left allocated",
                          players::SeatName(seat));
        }
    }
    g_installed = false;
    sources::Remove();  // before the add and the stop they route through
    RemoveRedirects();
}

void Tick() noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    bool any = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat))
        {
            g_memberType[seat].store(-1, std::memory_order_relaxed);
            g_copyOne[seat].store(false, std::memory_order_release);
            DropList(seat, OutText(seat));
            continue;
        }
        any                = true;
        void* const member = players::generation::MemberOf(seat);
        const auto  type   = member != nullptr ? session::party_member::PlayerTypeOf(member) : std::nullopt;
        g_memberType[seat].store(type ? static_cast<int>(*type) : -1, std::memory_order_relaxed);
        if (g_listTwo[seat].load(std::memory_order_relaxed) == nullptr && players::generation::ControllerOf(seat) != nullptr)
        {
            MakeList(seat);
        }
    }
    if (any)
    {
        LogSummary();
    }
}

void SeatOut(int seat) noexcept
{
    if (g_installed && players::generation::OnGameThread() && players::SeatIndex(seat))
    {
        CopyPlayerOne(seat, false);
        g_memberType[seat].store(-1, std::memory_order_relaxed);
        DropList(seat, LeavesText(seat));
    }
}

}  // namespace lss::feedback::traversal_rumble
