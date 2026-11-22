#include "rides/scour_scent.h"

#include "facts/party.h"
#include "facts/vehicles.h"
#include "facts/world.h"
#include "hud/passes/pass_opacity.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"
#include "rides/treasure_hint.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

#include <Windows.h>

namespace lss::rides::scour_scent {

void* Manager() noexcept
{
    return ReadPointer(players::game_globals::Object(players::game_globals::Global::GameRoot), facts::kRootChocoboCraft);
}

void* Session(const void* manager) noexcept
{
    return manager != nullptr ? const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(manager)) + facts::kCraftSession
                              : nullptr;
}

namespace {

using MgrTickFn    = void (*)(void* manager, float dt);
using GateFn       = bool (*)();
using LeaderFn     = void* (*)(bool fieldLeader);
using RideBodyFn   = void* (*)(std::uint8_t seatedOnly);

constexpr int           kNobody   = -1;
constexpr std::uint64_t kQuietMs  = 1000;   // a session idle this long has ended; an owner off their chocobo this long left it
constexpr std::uint64_t kStillMs  = 10000;  // a session quiet this long has ended, whatever its lock count says (the end line says "ten seconds")
constexpr std::size_t   kSniffs   = std::size(facts::kCraftSniffLeaderCalls);

// The sites Install found, placed from the game thread (the manager tick's call crosses a block).
struct Sites
{
    std::uintptr_t mgrTick = 0, mgrTickCall = 0;
    std::uintptr_t smellGate = 0, smellGateCall = 0, digGate = 0, digGateCall = 0;
    std::uintptr_t leader = 0, rideBody = 0;
    std::array<std::uintptr_t, kSniffs> sniffCalls{};
    std::uintptr_t locksOnCall = 0, locksOffCall = 0;
};
Sites                   g_sites;
platform::DeferredSites g_placement;

platform::CallRedirect                     g_mgrTickCall, g_smellGateCall, g_digGateCall, g_locksOnCall, g_locksOffCall;
std::array<platform::CallRedirect, kSniffs> g_sniffCalls;

// The session's owner: kNobody, 0 player 1, a seat. Written on the game thread; read on any (the sniff action's and the
// field locks' answers).
std::atomic<int> g_owner{kNobody};
// The field locks taken and given back in the owner's session (counted at the two ride reads; any thread), and the
// chocobo the on read answered, so the off read releases the same one.
std::atomic<std::uint32_t> g_locksOn{0}, g_locksOff{0};
std::atomic<void*>         g_lockedChocobo{nullptr};
// Once a session: each answer's first use said (bit per site; any thread).
std::atomic<std::uint32_t> g_answerSaid{0};

int Owner() noexcept
{
    return g_owner.load(std::memory_order_acquire);
}

/// The session has no owner any more.
void Free() noexcept
{
    g_owner.store(kNobody, std::memory_order_release);
}

/// A field lock taken or given back while a session is owned, counted for its end.
void CountLock(std::atomic<std::uint32_t>& count) noexcept
{
    if (Owner() != kNobody)
    {
        count.fetch_add(1, std::memory_order_relaxed);
    }
}

// Game thread.
std::uint64_t g_quietSince = 0;  // when the owner's session was first seen idle (0: not idle)
std::uint64_t g_stillSince = 0;  // ... idle but for its lock count
std::uint64_t g_offSince   = 0;  // ... with a seat's owner off their chocobo
bool          g_scopeSaid  = false, g_refusedSaid = false;
int           g_dimSaid    = kNobody;  // the owner whose session last dimmed another player's rows (logged once)
std::uint8_t  g_smellSaid  = 0;
hud::HiddenSet g_hidden;

const wchar_t* OwnerName(int owner) noexcept
{
    return owner >= 0 ? players::SeatName(owner) : L"nobody";
}

/// The owner, as an answer for the reads made outside the manager's tick: a player 2's or 3's session only.
int SeatOwner() noexcept
{
    const int owner = Owner();
    return owner >= 1 && owner <= players::kSeatsBuilt ? owner : kNobody;
}

void SayAnswerOnce(unsigned bit, const wchar_t* what, int seat) noexcept
{
    if ((g_answerSaid.fetch_or(1u << bit, std::memory_order_relaxed) & (1u << bit)) != 0)
    {
        return;
    }
    try
    {
        platform::LogDebug(L"Scour and Scent: {} answered {}'s ({}; said once a session)", what, players::SeatName(seat),
                           players::generation::OnGameThread() ? L"on the game thread" : L"on another thread");
    }
    catch (...)
    {
    }
}

/// A new session's owner: its counts and once-said marks start again.
void Own(int owner, const wchar_t* how) noexcept
{
    g_locksOn.store(0, std::memory_order_relaxed);
    g_locksOff.store(0, std::memory_order_relaxed);
    g_answerSaid.store(0, std::memory_order_relaxed);
    g_lockedChocobo.store(nullptr, std::memory_order_relaxed);
    g_quietSince = g_stillSince = g_offSince = 0;
    g_scopeSaid  = false;
    g_smellSaid  = 0;
    g_owner.store(owner, std::memory_order_release);
    try
    {
        platform::Log(L"Scour and Scent: {} {}: the one session is theirs (their chocobo, their member, their view) until it ends",
                      OwnerName(owner), how);
    }
    catch (...)
    {
    }
}

/// How long `since` has held `now` while `holds` (0 when it does not; set when it starts).
std::uint64_t HeldFor(std::uint64_t& since, bool holds, std::uint64_t now) noexcept
{
    since = !holds ? 0 : since == 0 ? now : since;
    return holds ? now - since : 0;
}

/// The owner's session has ended once nothing of it is pending, for kQuietMs: no request, smell mode off, every field
/// lock it took given back, the item's timers run out; or for kStillMs whatever the lock count says (a lock the game
/// never gave back must not keep the other players' rows dim). A seat's owner who left the game ends it at once, and one
/// off their chocobo for kQuietMs (their chocobo is gone; the session runs on as the game's).
void FollowRelease(const void* manager) noexcept
{
    const int owner = Owner();
    if (owner == kNobody)
    {
        return;
    }
    const void*         session = Session(manager);
    const std::uint8_t  smell   = ReadField<std::uint8_t>(session, facts::kCraftSmellMode);
    if (smell != g_smellSaid)
    {
        g_smellSaid = smell;
        platform::LogDebug(L"Scour and Scent: smell mode {} ({}'s session)", smell != 0 ? L"on" : L"off", OwnerName(owner));
    }
    const std::uint32_t on    = g_locksOn.load(std::memory_order_relaxed);
    const std::uint32_t off   = g_locksOff.load(std::memory_order_relaxed);
    const bool          still = ReadField<std::uint8_t>(manager, facts::kCraftScourRequest) == 0 &&
                       ReadField<std::uint8_t>(manager, facts::kCraftScentRequest) == 0 && smell == 0 &&
                       ReadField<float>(session, facts::kCraftItemTimer) <= 0.0f &&
                       ReadField<float>(session, facts::kCraftDoneTimer) <= 0.0f;
    const std::uint64_t now  = GetTickCount64();
    const bool          left = owner >= 1 && !players::generation::Active(owner);
    const bool offRide = owner >= 1 && HeldFor(g_offSince, players::generation::ChocoboOf(owner) == nullptr, now) >= kQuietMs;
    const bool idle    = HeldFor(g_quietSince, still && on <= off, now) >= kQuietMs;
    const bool stuck   = HeldFor(g_stillSince, still, now) >= kStillMs;
    if (!left && !offRide && !idle && !stuck)
    {
        return;
    }
    Free();
    g_quietSince = g_stillSince = g_offSince = 0;
    g_dimSaid    = kNobody;
    const wchar_t* why = left      ? L" (they left the game: the session is the game's from here)"
                         : offRide ? L" (they are off their chocobo: the session is the game's from here)"
                         : !idle   ? L" (quiet for ten seconds with a field lock not given back)"
                                   : L"";
    platform::Log(L"Scour and Scent: {}'s session ended{} ({} field lock(s) taken, {} given back){}", OwnerName(owner), why, on,
                  off, still ? L"" : L"; a request or the item's timer was still pending");
}
struct TickCall
{
    MgrTickFn fn;
    void*     manager;
    float     dt;
};

void RunTick(void* context) noexcept
{
    const auto& c = *static_cast<TickCall*>(context);
    c.fn(c.manager, c.dt);
}

/// The manager's tick inside `seat`'s primary scope and leader answer; false, nothing called, when refused.
bool TickAsSeat(int seat, TickCall& call) noexcept
{
    void*                  member = players::generation::MemberOf(seat);
    scopes::seat_ladder::Args args;
    args.seat = seat;
    if (member == nullptr || !scopes::seat_ladder::FillMember(args, member))
    {
        return false;
    }
    const scopes::seat_ladder::SeatScopes scopes(scopes::seat_ladder::Primary | scopes::seat_ladder::Leader, args);
    if (!scopes.Held())
    {
        return false;
    }
    RunTick(&call);
    return true;
}

/// Replaces the manager tick's call: a request with nobody named is player 1's press; a player 2's or 3's session runs
/// in their scopes; then the session's end is followed.
void MgrTick(void* manager, float dt) noexcept
{
    const auto original = reinterpret_cast<MgrTickFn>(g_mgrTickCall.Original());
    if (manager == nullptr || !players::generation::OnGameThread())
    {
        original(manager, dt);
        return;
    }
    const bool requested = ReadField<std::uint8_t>(manager, facts::kCraftScourRequest) != 0 ||
                           ReadField<std::uint8_t>(manager, facts::kCraftScentRequest) != 0;
    if (requested && Owner() == kNobody && players::generation::AnyActive())
    {
        Own(0, L"pressed Scent or Scour");
    }
    TickCall  call{original, manager, dt};
    const int seat  = treasure_hint::BeforeTick(manager, Owner());
    int       ranAs = 0;  // whose answers the tick ran in (player 1's when a seat's were refused)
    if (seat >= 1 && players::generation::Active(seat))
    {
        if (TickAsSeat(seat, call))
        {
            ranAs = seat;
            if (!g_scopeSaid)
            {
                g_scopeSaid = true;
                platform::LogDebug(L"Scour and Scent: the craft's tick runs in {}'s scopes (the party leader their member, "
                                   L"the primary controller theirs)",
                                   players::SeatName(seat));
            }
        }
        else
        {
            if (!g_refusedSaid)
            {
                g_refusedSaid = true;
                platform::LogWarning(L"Scour and Scent: {}'s scopes were refused for the craft's tick: it runs as the game's "
                                     L"(player 1's chocobo) this tick (said once)",
                                     players::SeatName(seat));
            }
            original(manager, dt);
        }
    }
    else
    {
        original(manager, dt);
    }
    FollowRelease(manager);
    treasure_hint::AfterTick(manager, ranAs);
}

/// A row gate in a chocobo's input slot: for the rider whose scope is open, the game's answer in their leader answer;
/// while another player's session runs, no (the game's own dim row).
bool Gate(platform::CallRedirect& call, const wchar_t* row) noexcept
{
    const auto original = reinterpret_cast<GateFn>(call.Original());
    if (!players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        return original();
    }
    const int rider = scopes::primary_controller::OpenSeat();
    const int owner = Owner();
    if (owner != kNobody && owner != rider)
    {
        if (g_dimSaid != owner)
        {
            g_dimSaid = owner;
            platform::LogDebug(L"Scour and Scent: {}'s {} row dims: {}'s session runs (said once a session)",
                               players::SeatName(rider), row, players::SeatName(owner));
        }
        return false;
    }
    if (rider < 1)
    {
        return original();
    }
    const scopes::leader_scope::LeaderScope leader(players::generation::MemberWeakOf(rider));
    return original();
}

bool SmellGate() noexcept
{
    return Gate(g_smellGateCall, L"Scent");
}

bool DigGate() noexcept
{
    return Gate(g_digGateCall, L"Scour");
}

/// The sniff action's reads of the party leader: a player 2's or 3's session's member.
template <std::size_t I>
void* SniffLeader(bool fieldLeader) noexcept
{
    const auto original = reinterpret_cast<LeaderFn>(g_sniffCalls[I].Original());
    const int  seat     = fieldLeader ? kNobody : SeatOwner();
    void*      member   = seat >= 1 ? players::generation::MemberOf(seat) : nullptr;
    if (member == nullptr)
    {
        return original(fieldLeader);
    }
    SayAnswerOnce(static_cast<unsigned>(I), L"the sniff action's read of the party leader", seat);
    return member;
}

/// The field locks' reads of the leader's chocobo (the forced walk set, then released): a player 2's or 3's session's
/// chocobo, each counted for the session's end. The release answers the chocobo the set answered, so the walk is never
/// set on one chocobo and released on another.
void* LocksOnRide(std::uint8_t seatedOnly) noexcept
{
    const auto original = reinterpret_cast<RideBodyFn>(g_locksOnCall.Original());
    CountLock(g_locksOn);
    const int seat    = SeatOwner();
    void*     chocobo = seat >= 1 ? const_cast<void*>(players::generation::ChocoboOf(seat)) : nullptr;  // the game's own
    if (chocobo == nullptr)
    {
        return original(seatedOnly);
    }
    g_lockedChocobo.store(chocobo, std::memory_order_relaxed);
    SayAnswerOnce(kSniffs, L"the field locks' read of the leader's chocobo", seat);
    return chocobo;
}

void* LocksOffRide(std::uint8_t seatedOnly) noexcept
{
    const auto original = reinterpret_cast<RideBodyFn>(g_locksOffCall.Original());
    CountLock(g_locksOff);
    // The chocobo the set answered while it is still the owner's; else their chocobo now (none: the game's own read).
    const int   seat    = SeatOwner();
    void*       locked  = g_lockedChocobo.exchange(nullptr, std::memory_order_relaxed);
    const void* now     = seat >= 1 ? players::generation::ChocoboOf(seat) : nullptr;
    void*       chocobo = locked != nullptr && locked == now ? locked : const_cast<void*>(now);
    if (chocobo == nullptr)
    {
        return original(seatedOnly);
    }
    SayAnswerOnce(kSniffs + 1, L"the field locks' release of the leader's chocobo", seat);
    return chocobo;
}

template <std::size_t... I>
bool PlaceSniffs(std::index_sequence<I...>) noexcept
{
    return (g_placement.Place(g_sniffCalls[I], g_sites.sniffCalls[I], g_sites.leader,
                              reinterpret_cast<std::uintptr_t>(&SniffLeader<I>)) &&
            ...);
}

void RestoreAll() noexcept
{
    bool left = false;
    for (platform::CallRedirect* call : {&g_mgrTickCall, &g_smellGateCall, &g_digGateCall, &g_locksOnCall, &g_locksOffCall})
    {
        left = (call->IsInstalled() && !call->Restore()) || left;
    }
    for (platform::CallRedirect& call : g_sniffCalls)
    {
        left = (call.IsInstalled() && !call.Restore()) || left;
    }
    if (left)
    {
        platform::Log(L"Scour and Scent: a call was changed after us; left in place (it passes through)");
    }
}

/// Every site or none: a session half answered would put a player 2's sniff on player 1's member.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s  = g_sites;
    const bool   ok = s.mgrTick != 0 && s.smellGate != 0 && s.digGate != 0 && s.leader != 0 && s.rideBody != 0 &&
                    g_placement.Place(g_mgrTickCall, s.mgrTickCall, s.mgrTick, reinterpret_cast<std::uintptr_t>(&MgrTick)) &&
                    g_placement.Place(g_smellGateCall, s.smellGateCall, s.smellGate, reinterpret_cast<std::uintptr_t>(&SmellGate)) &&
                    g_placement.Place(g_digGateCall, s.digGateCall, s.digGate, reinterpret_cast<std::uintptr_t>(&DigGate)) &&
                    PlaceSniffs(std::make_index_sequence<kSniffs>{}) &&
                    g_placement.Place(g_locksOnCall, s.locksOnCall, s.rideBody, reinterpret_cast<std::uintptr_t>(&LocksOnRide)) &&
                    g_placement.Place(g_locksOffCall, s.locksOffCall, s.rideBody, reinterpret_cast<std::uintptr_t>(&LocksOffRide));
    if (!ok)
    {
        RestoreAll();
    }
    else
    {
        g_hidden.Reserve(4);  // the filter, the icon, the two arrows
    }
    platform::Log(L"Scour and Scent: {}",
                  ok ? L"players 2 and 3's Scent and Scour run the game's craft for their own chocobo, one session at a time "
                       L"(the others' rows dim while it runs)"
                     : L"UNAVAILABLE (a check failed): a player 2's or 3's Scent or Scour acts on player 1's chocobo");
    return ok;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site = [&](std::uintptr_t address) { return image.Rebase(address, 5).value_or(0); };
    Sites      s;
    s.mgrTick       = platform::FindFunction(image, facts::kCraftMgrTick).value_or(0);
    s.smellGate     = platform::FindFunction(image, facts::kCraftSmellGate).value_or(0);
    s.digGate       = platform::FindFunction(image, facts::kCraftDigGate).value_or(0);
    s.leader        = platform::FindFunction(image, facts::kResolveFieldLeader).value_or(0);
    s.rideBody      = platform::FindFunction(image, facts::kLeaderRideBody).value_or(0);
    s.mgrTickCall   = site(facts::kCraftMgrTickCall);
    s.smellGateCall = site(facts::kCraftSmellGateCall);
    s.digGateCall   = site(facts::kCraftDigGateCall);
    for (std::size_t i = 0; i < kSniffs; ++i)
    {
        s.sniffCalls[i] = site(facts::kCraftSniffLeaderCalls[i]);
    }
    s.locksOnCall  = site(facts::kCraftLocksOnRideCall);
    s.locksOffCall = site(facts::kCraftLocksOffRideCall);
    if (players::game_globals::Address(players::game_globals::Global::GameRoot) == 0 || !scopes::leader_scope::Installed())
    {
        platform::Log(L"Scour and Scent: an engine check failed; a player 2's or 3's Scent or Scour acts on player 1's chocobo");
        return false;
    }
    g_sites = s;
    // The manager tick's call crosses a 16-byte block in 1.005: every site is placed from the game thread, between its
    // ticks (platform/deferred_sites.h).
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread() && !placed)
    {
        platform::Log(L"Scour and Scent: UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();
    RestoreAll();
    Free();
    g_hidden.Forget();
}

PressWatch::PressWatch(int seat) noexcept
{
    // A session already owned names no press; a request already pending is someone else's (player 1's, named at the
    // manager's tick), so a press made beside it in this tick is not taken as the owner's either.
    const void* manager = players::SeatBuilt(seat) && g_mgrTickCall.IsInstalled() &&
                                  Owner() == kNobody
                              ? Manager()
                              : nullptr;
    if (manager == nullptr || ReadField<std::uint8_t>(manager, facts::kCraftScourRequest) != 0 ||
        ReadField<std::uint8_t>(manager, facts::kCraftScentRequest) != 0)
    {
        return;
    }
    seat_ = seat;
}

PressWatch::~PressWatch()
{
    if (seat_ < 1 || Owner() != kNobody)
    {
        return;
    }
    const void* manager = Manager();
    const bool  scour   = ReadField<std::uint8_t>(manager, facts::kCraftScourRequest) != 0;
    const bool  scent   = ReadField<std::uint8_t>(manager, facts::kCraftScentRequest) != 0;
    if (scour || scent)
    {
        Own(seat_, scent ? L"pressed Scent" : L"pressed Scour");
    }
}

void ShowOnly(int seat) noexcept
{
    // A copy of player 1's picture (a mirror, a seat on player 1's camera) is taken as its seat's pass, as the jet
    // gauges take it: the owner's widgets are placed from the owner's view, so they belong in that one view alone.
    Restore();
    if (seat == treasure_hint::SessionSetPlayer() || !players::widget_opacity::Installed() ||
        !players::generation::OnGameThread())
    {
        return;
    }
    const void* session = Session(Manager());
    if (session == nullptr)
    {
        return;
    }
    for (const std::uint32_t field : {facts::kCraftFilter, facts::kCraftStatusIcon, facts::kCraftArrowA, facts::kCraftArrowB})
    {
        (void)g_hidden.Hide(players::weak_object::ObjectIn(session, field), ReadField<std::uint64_t>(session, field));
    }
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::rides::scour_scent
