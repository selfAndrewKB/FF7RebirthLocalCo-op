#include "rides/chocobo_call.h"

#include "facts/vehicles.h"
#include "platform/call_redirect.h"
#include "platform/code_memory.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/command_registry.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "rides/chocobo_ride.h"
#include "scopes/primary_controller.h"
#include "session/party_member.h"
#include "world/data_rows.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <Windows.h>
#include <intrin.h>

namespace lss::rides::chocobo_call {

namespace {

using WhistleFn    = void (*)(void* state, bool stay);
using ForKeyFn     = bool (*)(const std::uint8_t* request);
using BuggyFn      = bool (*)(void* character, void* a, void* b, void* c);
using NotifyFn     = void (*)(void* self, void* mesh, void* animation, void* extra);
using FromWhistleFn = void (*)(bool rideOnArrival);
using PartyRideFn  = bool (*)(void* manager);
using LabelFn      = bool (*)(void* state);
using GetOffFn     = void (*)(void* request);
using ActionFn     = void* (*)(void* actorInterface, std::uint64_t name);
using AcceptFn     = bool (*)(void* manager, bool fromWhistle);
using SoundAssetFn = void* (*)(const std::uint64_t* name);
using SoundPlayFn  = void (*)(void* asset, const std::uint64_t* name);
using BrainFn      = void (*)(void* ai, int kind, void* params, int extra);
using IconFn       = bool (*)(void* coordinator, void* character, bool trace, std::uint8_t* blocked);

constexpr std::size_t   kForKeys     = std::size(facts::kSummonRideForKeyCalls);
constexpr std::size_t   kBuggies     = std::size(facts::kSummonCanCallBuggyCalls);
constexpr std::size_t   kEntryCalls  = std::size(facts::kEntryPartyRideCalls);
constexpr std::uint64_t kGiveUpMs    = 15000;  // a call whose member is not riding by then is given up
constexpr std::uint64_t kRestoreMs   = 10000;  // after a field start, how long a restored ride may still be taking them
// A member entry's states.
constexpr int kEntryIdle = 0, kEntryNamed = 1, kEntryPlaced = 3, kEntryRiding = 4, kEntryDismounting = 5;

// The rider's pose at a chocobo stop, for a rider without the rider's stop animations (all but Cloud and Tifa): the
// chocobo's stop animation names, start, hold, success and cancel, and what each falls back to, the rein pull of
// every member's riding set (its start, its hold loop, its end).
constexpr std::array<const wchar_t*, 4> kRiderStopNames{L"N_ChocoboStop02_0", L"N_ChocoboStop02_1", L"N_ChocoboStop02_2",
                                                        L"N_ChocoboStop02_3"};
constexpr std::array<const wchar_t*, 4> kRiderStopFallbacks{L"N_Brake01_0", L"N_Brake01_1", L"N_Brake01_2", L"N_Brake01_2"};
constexpr std::uint64_t kRiderAskGapMs   = 500;   // the rider's action asks each tick; a gap this long starts a new pose
constexpr std::uint64_t kRiderAskStaleMs = 2000;  // a rider record unasked this long is free for another rider

// What Install found, placed from the game thread (the party getoff's call crosses a block).
struct Sites
{
    std::uintptr_t whistle = 0, whistleCall = 0, forKey = 0, buggy = 0, fromWhistle = 0, fromWhistleCall = 0;
    std::uintptr_t partyRide = 0, suspended = 0, getOff = 0, getOffCall = 0, stayLabel = 0, stayLabelCall = 0;
    std::uintptr_t findAnim = 0, rowReturn = 0, accept = 0, acceptCall = 0, soundAsset = 0, soundPlay = 0, brain = 0, followCall = 0;
    std::uintptr_t riderReturn = 0, icon = 0, iconCall = 0;
    std::array<std::uintptr_t, kForKeys>    forKeyCalls{};
    std::array<std::uintptr_t, kBuggies>    buggyCalls{};
    std::array<std::uintptr_t, kEntryCalls> entryCalls{};
};
Sites                   g_sites;
std::optional<platform::GameImage> g_image;  // for the notify slot, placed with the sites
platform::DeferredSites g_placement;

platform::CallRedirect                         g_whistleCall, g_fromWhistleCall, g_getOffCall, g_stayLabelCall;
platform::CallRedirect                         g_acceptCall, g_followCall, g_iconCall;
platform::VTableEntry                          g_findAnimSlot, g_caitSithFindAnimSlot;
std::array<platform::CallRedirect, kForKeys>    g_forKeyCalls;
std::array<platform::CallRedirect, kBuggies>    g_buggyCalls;
std::array<platform::CallRedirect, kEntryCalls> g_entryCalls;
platform::VTableEntry                          g_notifySlot;

// By seat: the player's own call stands (their whistle reached the call; until their getoff, their leaving, or the
// call given up). Written on the game thread; read by the ride module's tick (the same thread).
std::array<bool, players::kMaxPlayers>          g_called{};
std::array<std::uint64_t, players::kMaxPlayers> g_calledAt{};
static_assert(players::kMaxPlayers == 3, "a value per player below");
std::array<int, players::kMaxPlayers>           g_entrySaid{-2, -2, -2};  // a calling player's entry state as last said
std::array<bool, players::kMaxPlayers>          g_riddenSaid{};             // their member rode in this call
std::array<const void*, players::kMaxPlayers>   g_calledMember{};           // the member their call is for

// By seat, a ride the game put them on that becomes their own call (game thread): the member the join window was opened
// for (null while they are out of the game), that window (a ride already under them as they join or take a member),
// the end of a field start's window (a ride restored with a save, still seating them), and a story party mount pending.
std::array<const void*, players::kMaxPlayers>   g_windowMember{};
std::array<bool, players::kMaxPlayers>          g_window{};
std::array<std::uint64_t, players::kMaxPlayers> g_restoreUntil{};
std::array<bool, players::kMaxPlayers>          g_story{};
std::array<int, players::kMaxPlayers>           g_leaderSeen{-1, -1, -1};  // the leader state at their last answer
std::array<bool, players::kMaxPlayers>          g_iconSaid{};  // their idle coordinator's icon question answered, said once

// The rider's stop pose names (index as kRiderStopNames): found on the game thread once each exists, read by the rider's
// action on any thread.
std::array<std::atomic<std::uint64_t>, kRiderStopNames.size()> g_riderAsked{}, g_riderFallback{};

// One record per rider whose stop pose the mod answered (any thread): the rider's character interface, the name last
// answered, the time of its last ask.
struct RiderAsk
{
    std::atomic<std::uintptr_t> rider{0};
    std::atomic<std::uint64_t>  name{0};
    std::atomic<std::uint64_t>  at{0};
};
std::array<RiderAsk, players::kMaxPlayers + 1> g_riderAsks{};

// The option own-chocobo (the console and the page set it, on the game thread; off: players 2 and 3 ride along with
// player 1's party ride, as in single player).
std::atomic<bool> g_ownOn{false};

// Game thread: the player whose summon body runs (0: none), and the player whose member's whistle notify runs (0: none).
int  g_summonSeat = 0;
int  g_notifySeat = 0;
bool g_rowByMod   = false;  // the summon row's action check was the mod's answer (the member has no whistle action)

/// The player whose scope is open on the game thread (0 for none, or off it).
int OpenSeatHere() noexcept
{
    return players::generation::OnGameThread() ? scopes::primary_controller::OpenSeat() : 0;
}

/// A seat's member's player type, or -1.
int TypeOf(int seat) noexcept
{
    const auto type = session::party_member::PlayerTypeOf(players::generation::MemberOf(seat));
    return type ? static_cast<int>(*type) : -1;
}

/// `seat`'s windows for a ride the game put them on close: their own press, or the end of their call, decides from here.
void CloseWindows(int seat) noexcept
{
    const auto index      = static_cast<std::size_t>(seat);
    g_window[index]       = false;
    g_restoreUntil[index] = 0;
}

void EndCall(int seat, const wchar_t* why) noexcept
{
    const auto index = static_cast<std::size_t>(seat);
    if (!g_called[index])
    {
        return;
    }
    CloseWindows(seat);
    g_called[index]       = false;
    g_entrySaid[index]    = -2;
    g_riddenSaid[index]   = false;
    g_calledMember[index] = nullptr;
    platform::Log(L"chocobo call: {}'s own call ends ({}): {}", players::SeatName(seat), why,
                  g_ownOn.load(std::memory_order_relaxed) ? L"they ride again when they call their own chocobo"
                                                          : L"they ride along with player 1's party ride");
}

/// `seat`'s own call starts (or starts again): their member's entry asks for a chocobo of its own.
void StartCall(int seat, const wchar_t* how) noexcept
{
    const auto index = static_cast<std::size_t>(seat);
    const bool again = g_called[index];
    CloseWindows(seat);
    g_called[index]       = true;
    g_calledAt[index]     = GetTickCount64();
    g_calledMember[index] = players::generation::MemberOf(seat);
    const void* manager   = chocobo_ride::RideManagerNow();
    platform::Log(L"chocobo call: {} calls their own chocobo{} ({}; the ride manager's assets {})", players::SeatName(seat),
                  again ? L" again" : L"", how,
                  ReadField<std::uint8_t>(manager, facts::kRideManagerAssets) == facts::kRideAssetsLoaded ? L"loaded"
                                                                                                            : L"NOT loaded yet");
}

/// `seat`'s member already rides a chocobo the game seated them on: it becomes their own call (their Vehicle press gets
/// them off, player 1's getoff leaves them on).
void AdoptCall(int seat, const wchar_t* why) noexcept
{
    const auto index      = static_cast<std::size_t>(seat);
    g_called[index]       = true;
    g_calledAt[index]     = GetTickCount64();
    g_calledMember[index] = players::generation::MemberOf(seat);
    g_riddenSaid[index]   = true;
    g_entrySaid[index]    = kEntryRiding;
    g_story[index]        = false;
    CloseWindows(seat);
    platform::Log(L"chocobo call: {} keeps the ride the game seated them on ({}): it is their own now (their Vehicle press "
                  L"gets them off)",
                  players::SeatName(seat), why);
}

/// The rider's stop pose for a rider without the rider's stop animations: the fallback for the name the rider's action
/// asked, looked up on the same character interface, once for each new name (the action asks each tick while its pose
/// differs from the chocobo's, and every answer would start the clip again); null for a repeat, so the clip it plays keeps
/// on. Any thread.
void* RiderStopFallback(void* actorInterface, std::uint64_t asked) noexcept
{
    std::size_t which = kRiderStopNames.size();
    for (std::size_t i = 0; i < kRiderStopNames.size(); ++i)
    {
        if (asked != 0 && g_riderAsked[i].load(std::memory_order_relaxed) == asked)
        {
            which = i;
        }
    }
    const std::uint64_t fallback = which < kRiderStopNames.size() ? g_riderFallback[which].load(std::memory_order_relaxed) : 0;
    if (fallback == 0)
    {
        return nullptr;
    }
    const std::uint64_t now  = GetTickCount64();
    const auto          key  = reinterpret_cast<std::uintptr_t>(actorInterface);
    RiderAsk*           slot = nullptr;
    for (RiderAsk& r : g_riderAsks)
    {
        if (slot == nullptr && r.rider.load(std::memory_order_acquire) == key)
        {
            slot = &r;
        }
    }
    for (RiderAsk& r : g_riderAsks)
    {
        std::uintptr_t      held  = r.rider.load(std::memory_order_acquire);
        const std::uint64_t asked = r.at.load(std::memory_order_relaxed);
        const bool          stale = held == 0 || (asked < now && now - asked > kRiderAskStaleMs);
        if (slot == nullptr && stale && r.rider.compare_exchange_strong(held, key, std::memory_order_acq_rel))
        {
            r.at.store(now, std::memory_order_relaxed);  // at once: another rider never takes it as stale
            r.name.store(0, std::memory_order_relaxed);
            slot = &r;
        }
    }
    if (slot == nullptr)
    {
        return nullptr;  // more riders than records: nothing played for this one
    }
    const bool repeat = slot->name.load(std::memory_order_relaxed) == asked &&
                        now - slot->at.load(std::memory_order_relaxed) < kRiderAskGapMs;
    slot->at.store(now, std::memory_order_relaxed);
    if (repeat)
    {
        return nullptr;
    }
    slot->name.store(asked, std::memory_order_relaxed);
    // A rider without the fallback either shows no pose, as before; its repeats answer null like any other.
    void* anim = reinterpret_cast<ActionFn>(g_sites.findAnim)(actorInterface, fallback);
    platform::LogDebug(L"chocobo call: a rider without the rider's chocobo stop animations asked {}: {} {}", kRiderStopNames[which],
                       anim != nullptr ? L"plays" : L"has no", kRiderStopFallbacks[which]);
    return anim;
}

/// The whistle notify's sound, for a whistle without its animation (and so without its notify).
void PlayWhistleSound() noexcept
{
    static std::uint64_t name = 0;
    name = name != 0 ? name : world::data_rows::FindName(L"se_sys_chocobo_whistle");
    void* asset = nullptr;
    if (name == 0 || !platform::CallGuarded(reinterpret_cast<SoundAssetFn>(g_sites.soundAsset), asset,
                                            static_cast<const std::uint64_t*>(&name)) ||
        asset == nullptr)
    {
        return;
    }
    platform::CallGuarded(reinterpret_cast<SoundPlayFn>(g_sites.soundPlay), asset, static_cast<const std::uint64_t*>(&name));
}

/// The character interface's find-animation-by-name (its vtable slot), known by the return address: the game's answer;
/// for the summon row's check that the character has the whistle action, inside a player 2's or 3's summon body, "found"
/// when their member has none (the answer is only tested for null), so their row and press work like Cloud's and
/// Tifa's; for a rider's pose at a chocobo stop, a riding clip of their own when they have none of the stop's.
void* FindAnim(void* actorInterface, std::uint64_t name) noexcept
{
    void*      found = reinterpret_cast<ActionFn>(g_sites.findAnim)(actorInterface, name);
    const auto from  = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (from == g_sites.riderReturn && from != 0)
    {
        return found != nullptr ? found : RiderStopFallback(actorInterface, name);
    }
    if (from != g_sites.rowReturn || g_summonSeat < 1 ||
        !players::generation::OnGameThread())
    {
        return found;
    }
    g_rowByMod = found == nullptr;
    return found != nullptr ? found : actorInterface;
}

/// Inside a player 2's or 3's summon body the ride manager takes their call whatever player 1's chocobo does: their call
/// asks for their member's own chocobo, not the leader's.
bool AcceptCall(void* manager, bool fromWhistle) noexcept
{
    const auto original = reinterpret_cast<AcceptFn>(g_acceptCall.Original());
    return g_summonSeat >= 1 && players::generation::OnGameThread() ? true : original(manager, fromWhistle);
}

/// Inside a player 2's or 3's summon body, the "stay" order would act on player 1's chocobo: theirs is always the whistle
/// (the row no longer reads "stay" for them; this holds if a buffered press still arrives with it).
void Whistle(void* state, bool stay) noexcept
{
    const auto original = reinterpret_cast<WhistleFn>(g_whistleCall.Original());
    if (g_summonSeat >= 1 && players::generation::OnGameThread())
    {
        if (g_rowByMod)
        {
            // No whistle action for their member: the game's whistle would push nothing and send no call. The press does
            // what the whistle's notify does: its sound, then the call.
            PlayWhistleSound();
            StartCall(g_summonSeat, L"their press; their member has no whistle animation");
            return;
        }
        platform::LogDebug(L"chocobo call: {} whistles{}", players::SeatName(g_summonSeat),
                           stay ? L" (the row read \"stay\" for player 1's chocobo: theirs is the whistle)" : L"");
        original(state, false);
        return;
    }
    original(state, stay);
}

/// Inside a player 2's or 3's summon body: no walk-in ride, scooter or buggy ride (party vehicles, or the scooter's own
/// module's). The redirect's three sites share it by index.
template <std::size_t I>
bool ForKey(const std::uint8_t* request) noexcept
{
    const auto original = reinterpret_cast<ForKeyFn>(g_forKeyCalls[I].Original());
    return g_summonSeat >= 1 && players::generation::OnGameThread() ? false : original(request);
}

/// Inside a player 2's or 3's summon body the row never reads "stay" (that order is for player 1's chocobo): their press
/// is the whistle, and the row says so.
bool StayLabel(void* state) noexcept
{
    const auto original = reinterpret_cast<LabelFn>(g_stayLabelCall.Original());
    return g_summonSeat >= 1 && players::generation::OnGameThread() ? false : original(state);
}

/// Inside a player 2's or 3's summon body: no buggy row or press (the party's buggy).
template <std::size_t I>
bool Buggy(void* character, void* a, void* b, void* c) noexcept
{
    const auto original = reinterpret_cast<BuggyFn>(g_buggyCalls[I].Original());
    return g_summonSeat >= 1 && players::generation::OnGameThread() ? false : original(character, a, b, c);
}

/// The whistle's notify: the game's (its sound, the story gate), with the whistling character's player named for the
/// call inside it.
void Notify(void* self, void* mesh, void* animation, void* extra) noexcept
{
    const auto original = reinterpret_cast<NotifyFn>(g_notifySlot.Original());
    const int  saved    = g_notifySeat;
    if (players::generation::OnGameThread() && players::generation::AnyActive())
    {
        // With own-chocobo off a player 2's or 3's whistle (a story one) is the game's party call, as player 1's.
        const int seat = g_ownOn.load(std::memory_order_relaxed)
                             ? players::generation::SeatOfMember(players::actor_fields::ComponentOwner(mesh))
                             : -1;
        g_notifySeat   = players::SeatBuilt(seat) && players::generation::Active(seat) ? seat : 0;
    }
    original(self, mesh, animation, extra);
    g_notifySeat = saved;
}

/// The party's call from a whistle: a player 2's or 3's member's whistle is their own call instead.
void FromWhistle(bool rideOnArrival) noexcept
{
    const auto original = reinterpret_cast<FromWhistleFn>(g_fromWhistleCall.Original());
    const int  seat     = g_notifySeat;
    if (seat < 1)
    {
        original(rideOnArrival);
        return;
    }
    StartCall(seat, L"their whistle; the party's call skipped");
}

/// The ride manager's leader is boarding or riding.
bool LeaderRides(std::uint8_t leader) noexcept
{
    return leader == facts::kRideLeaderArrived || leader == facts::kRideLeaderMounting || leader == facts::kRideLeaderRiding;
}

/// A playing player 2's or 3's member's entry asks "the party rides" (`game`: the game's answer). Their own call stands
/// for it, never player 1's ride, except where the game puts them on a ride on purpose, which then becomes their own:
/// - a story party mount: every member's chocobo spawns and waits for the story's cutscene, which seats them all. They
///   take the game's answer meanwhile; a ride already under them would keep the story waiting, so it ends first and the
///   game spawns them a chocobo like the others'. The cutscene's seat becomes their own call.
/// - a ride under them as they join or take a member (they joined on it), or a ride restored with a loaded save, which
///   seats the members after the field starts: the game's answer while it seats them, their own call once seated.
bool SeatAnswer(void* manager, int seat, bool game) noexcept
{
    const auto          index  = static_cast<std::size_t>(seat);
    const void*         member = players::generation::MemberOf(seat);
    const int           entry  = chocobo_ride::MemberEntryState(member);
    const auto          leader = ReadField<std::uint8_t>(manager, facts::kRideManagerLeaderState);
    const std::uint64_t now    = GetTickCount64();
    if (member != g_windowMember[index])
    {
        g_windowMember[index] = member;
        g_window[index]       = !g_called[index];
        platform::LogDebug(L"chocobo call: {} joined or took a member: a ride already under them is theirs (their entry "
                           L"state {}, the leader's {})",
                           players::SeatName(seat), entry, leader);
    }
    // A field start (the leader entering its first state): a ride restored with the save may seat them next.
    if (leader == facts::kRideLeaderFieldStart && g_leaderSeen[index] != facts::kRideLeaderFieldStart && !g_called[index])
    {
        g_restoreUntil[index] = now + kRestoreMs;
        platform::LogDebug(L"chocobo call: the field starts: a ride restored with the save may seat {} in the next {} s",
                           players::SeatName(seat), kRestoreMs / 1000);
    }
    g_leaderSeen[index] = leader;
    if (ReadField<std::uint8_t>(manager, facts::kRideManagerStoryMount) != 0)
    {
        if (!g_story[index])
        {
            platform::LogDebug(L"chocobo call: the story mounts the party: {} is seated with everyone (their entry state {})",
                               players::SeatName(seat), entry);
        }
        g_story[index] = true;
        if (entry == kEntryRiding || entry == kEntryDismounting)
        {
            EndCall(seat, L"the story mounts the party: they get off to be seated with everyone");
            return false;
        }
        return game;
    }
    if (g_story[index])
    {
        if (entry == kEntryRiding)
        {
            AdoptCall(seat, L"the story seated them");
        }
        else if (entry == kEntryPlaced)
        {
            return game;
        }
        else
        {
            g_story[index] = false;
        }
    }
    if (!g_called[index] && (g_window[index] || now < g_restoreUntil[index]))
    {
        const bool restored = ReadField<std::uint8_t>(manager, facts::kRideManagerInstant) != 0;
        if (entry == kEntryRiding)
        {
            AdoptCall(seat, restored ? L"a ride restored with the save" : L"they joined, or took this member, on a ride");
        }
        else if (restored && entry >= kEntryNamed && entry <= kEntryPlaced)
        {
            return game;
        }
        else if (restored && entry == kEntryIdle && now < g_restoreUntil[index] && LeaderRides(leader))
        {
            return game;
        }
        else if (g_window[index])
        {
            g_window[index] = false;
            platform::LogDebug(L"chocobo call: no ride under {} (their entry state {}): their own call decides",
                               players::SeatName(seat), entry);
        }
    }
    // Once their member rode, only while that ride lasts: an ended ride's entry asks again before the tick ends the call.
    return g_called[index] && !reinterpret_cast<PartyRideFn>(g_sites.suspended)(manager) &&
           ReadField<std::uint8_t>(manager, facts::kRideManagerRideOff) == 0 &&
           (!g_riddenSaid[index] || entry == kEntryRiding);
}

/// The member entries' "the party rides" question, for the entry at `offset` from the first. For a playing player 2's or
/// 3's member's entry, SeatAnswer (no mounting and no dismounting with player 1); the rest of the game's answer stands
/// there (no fight, abort or getoff in progress, the manager's ride-off byte clear). Every other entry (the AI
/// members'), the game's answer.
bool EntryPartyRide(void* manager, std::uintptr_t offset) noexcept
{
    const bool game = reinterpret_cast<PartyRideFn>(g_sites.partyRide)(manager);
    if (!players::generation::OnGameThread() || offset % facts::kRideManagerEntrySize != 0)
    {
        return game;
    }
    if (!g_ownOn.load(std::memory_order_relaxed))
    {
        return game;
    }
    const auto type = static_cast<int>(offset / facts::kRideManagerEntrySize);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && TypeOf(seat) == type)
        {
            return SeatAnswer(manager, seat, game);
        }
    }
    return game;
}

/// The idle chocobo coordinator's walk-in ride icon test, asked about the field leader. Inside a player 2's or 3's input
/// pass (their own coordinator, idle while they walk): "blocked", no icon, as the walk-in ride is player 1's and the
/// leader it asks about is player 1's member, whose chocobo would show the prompt under player 1. Player 1's coordinator
/// (outside any player's scope): the game's.
bool AutoRideIcon(void* coordinator, void* character, bool trace, std::uint8_t* blocked) noexcept
{
    const auto original = reinterpret_cast<IconFn>(g_iconCall.Original());
    const int  seat     = OpenSeatHere();
    if (!players::SeatBuilt(seat))
    {
        return original(coordinator, character, trace, blocked);
    }
    if (blocked != nullptr)
    {
        *blocked = 1;
    }
    const auto index = static_cast<std::size_t>(seat);
    if (!g_iconSaid[index])
    {
        g_iconSaid[index] = true;
        platform::LogDebug(L"chocobo call: {}'s idle chocobo coordinator asked for the walk-in mount icon over the party "
                           L"leader's chocobo: no icon (the walk-in ride is player 1's; said once)",
                           players::SeatName(seat));
    }
    return false;
}

/// A member chocobo's AI at its ride begin sends it to follow player 1's ride body: not the chocobo a player 2 or 3 is
/// about to drive (the game's own rule, no follow for the chocobo a player rides, one step early), so it waits by their
/// member instead of running toward player 1 during their mount.
void FollowRequest(void* ai, int kind, void* params, int extra) noexcept
{
    const auto original = reinterpret_cast<BrainFn>(g_followCall.Original());
    const int  seat     = players::generation::OnGameThread()
                              ? chocobo_ride::BoardsChocobo(players::weak_object::ObjectIn(ai, facts::kMountAiPawn))
                              : 0;
    if (seat < 1)
    {
        // While a player's own call stands, a follow order that passes says whose chocobo it was (the board check missed it).
        const bool calling = std::any_of(g_called.begin(), g_called.end(), [](bool called) { return called; });
        if (calling && players::generation::OnGameThread() && platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(L"chocobo call: a member chocobo's follow order to player 1 passes ({})",
                               players::NameOf(players::weak_object::ObjectIn(ai, facts::kMountAiPawn)));
        }
        original(ai, kind, params, extra);
        return;
    }
    platform::LogDebug(L"chocobo call: {}'s chocobo is not sent to follow player 1 as their member mounts (they drive it)",
                       players::SeatName(seat));
}

/// The stub each entry call goes to: the entry's offset (r15 at the four calls) as the second argument, then a jump to
/// EntryPartyRide. No call, no stack use.
std::uintptr_t BuildEntryStub(std::uintptr_t site) noexcept
{
    static constexpr std::uint8_t kMove[] = {0x4C, 0x89, 0xFA};  // mov rdx, r15
    return platform::PlaceMoveAndJump(site, kMove, reinterpret_cast<std::uintptr_t>(&EntryPartyRide));
}

/// The rider's Vehicle press (the party's getoff): a player 2's or 3's own call ends instead, so only their member gets
/// off (the entry's answer goes back to the game's).
void GetOff(void* request) noexcept
{
    const auto original = reinterpret_cast<GetOffFn>(g_getOffCall.Original());
    const int  seat     = OpenSeatHere();
    if (seat >= 1 && g_called[static_cast<std::size_t>(seat)])
    {
        EndCall(seat, L"their Vehicle press");
        return;
    }
    original(request);
}

template <std::size_t... I>
bool PlaceIndexed(std::index_sequence<I...>) noexcept
{
    const Sites& s = g_sites;
    return ((g_placement.Place(g_forKeyCalls[I], s.forKeyCalls[I], s.forKey, reinterpret_cast<std::uintptr_t>(&ForKey<I>))) && ...);
}

template <std::size_t... I>
bool PlaceBuggies(std::index_sequence<I...>) noexcept
{
    const Sites& s = g_sites;
    return ((g_placement.Place(g_buggyCalls[I], s.buggyCalls[I], s.buggy, reinterpret_cast<std::uintptr_t>(&Buggy<I>))) && ...);
}

bool PlaceEntries() noexcept
{
    for (std::size_t i = 0; i < kEntryCalls; ++i)
    {
        const std::uintptr_t stub = g_sites.entryCalls[i] != 0 ? BuildEntryStub(g_sites.entryCalls[i]) : 0;
        if (stub == 0 || !g_placement.Place(g_entryCalls[i], g_sites.entryCalls[i], g_sites.partyRide, stub))
        {
            return false;
        }
    }
    return true;
}

void RestoreAll() noexcept
{
    bool left = false;
    for (platform::VTableEntry* slot : {&g_findAnimSlot, &g_caitSithFindAnimSlot})
    {
        left = (slot->Original() != 0 && !slot->Restore()) || left;
    }
    for (platform::CallRedirect* call :
         {&g_whistleCall, &g_fromWhistleCall, &g_getOffCall, &g_stayLabelCall, &g_acceptCall, &g_followCall, &g_iconCall})
    {
        left = (call->IsInstalled() && !call->Restore()) || left;
    }
    const auto restore = [&left](std::span<platform::CallRedirect> calls) {
        for (platform::CallRedirect& call : calls)
        {
            left = (call.IsInstalled() && !call.Restore()) || left;
        }
    };
    restore(g_forKeyCalls);
    restore(g_buggyCalls);
    restore(g_entryCalls);
    if (g_notifySlot.Original() != 0)
    {
        left = !g_notifySlot.Restore() || left;
    }
    if (left)
    {
        platform::Log(L"chocobo call: a call or slot was changed after us; left in place (it passes through)");
    }
}

/// Every site or none: a whistle half routed would call player 1's chocobo, or seat nobody.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s  = g_sites;
    const bool   ok = s.whistle != 0 && s.forKey != 0 && s.buggy != 0 && s.fromWhistle != 0 && s.partyRide != 0 &&
                    s.suspended != 0 && s.getOff != 0 && s.stayLabel != 0 && g_image.has_value() &&
                    g_placement.Place(g_whistleCall, s.whistleCall, s.whistle, reinterpret_cast<std::uintptr_t>(&Whistle)) &&
                    g_placement.Place(g_stayLabelCall, s.stayLabelCall, s.stayLabel, reinterpret_cast<std::uintptr_t>(&StayLabel)) &&
                    s.accept != 0 && s.soundAsset != 0 && s.soundPlay != 0 && s.brain != 0 &&
                    g_placement.Place(g_acceptCall, s.acceptCall, s.accept, reinterpret_cast<std::uintptr_t>(&AcceptCall)) &&
                    g_placement.Place(g_followCall, s.followCall, s.brain, reinterpret_cast<std::uintptr_t>(&FollowRequest)) &&
                    s.findAnim != 0 && s.rowReturn != 0 &&
                    platform::ReplaceSlot(*g_image, facts::kFindAnimByNameSlot, reinterpret_cast<std::uintptr_t>(&FindAnim), g_findAnimSlot) &&
                    platform::ReplaceSlot(*g_image, facts::kCaitSithFindAnimByNameSlot, reinterpret_cast<std::uintptr_t>(&FindAnim),
                                          g_caitSithFindAnimSlot) &&
                    PlaceIndexed(std::make_index_sequence<kForKeys>{}) && PlaceBuggies(std::make_index_sequence<kBuggies>{}) &&
                    g_placement.Place(g_fromWhistleCall, s.fromWhistleCall, s.fromWhistle,
                                      reinterpret_cast<std::uintptr_t>(&FromWhistle)) &&
                    PlaceEntries() &&
                    g_placement.Place(g_getOffCall, s.getOffCall, s.getOff, reinterpret_cast<std::uintptr_t>(&GetOff)) &&
                    platform::ReplaceSlot(*g_image, facts::kCallNotifySlot, reinterpret_cast<std::uintptr_t>(&Notify), g_notifySlot);
    if (!ok)
    {
        RestoreAll();
        platform::Log(L"chocobo call: UNAVAILABLE (a check failed): players 2 and 3 do not whistle; they ride with the party as "
                      L"before");
        return false;
    }
    // Apart from the rest: the icon question stands alone (the walk-in ride's icon under player 1).
    const bool icon = s.icon != 0 && s.iconCall != 0 &&
                      g_placement.Place(g_iconCall, s.iconCall, s.icon, reinterpret_cast<std::uintptr_t>(&AutoRideIcon));
    platform::Log(L"chocobo call: {}", icon ? L"players 2 and 3's idle chocobo coordinators show no walk-in mount icon over "
                                              L"player 1's chocobo"
                                            : L"the walk-in mount icon question NOT placed (its call is not as the research "
                                              L"read it): the icon may show over player 1's chocobo while player 2 or 3 walks");
    platform::Log(L"chocobo call: ready: with own-chocobo on, players 2 and 3 whistle for their own chocobo (the game seats "
                  L"their member at their side; their Vehicle press dismounts them alone), keep a ride the game seats them on "
                  L"(a story mount, a loaded save, joining on a ride); player 1's whistle and the party's ride are the game's; "
                  L"a rider without the rider's chocobo stop animations pulls the reins at a stop");
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site = [&](std::uintptr_t address) { return image.Rebase(address, 5).value_or(0); };
    const auto find = [&](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact).value_or(0); };
    Sites      s;
    s.whistle         = find(facts::kWhistleOrDismiss);
    s.forKey          = find(facts::kRideForKey);
    s.buggy           = find(facts::kCanCallBuggy);
    s.fromWhistle     = find(facts::kCallFromWhistle);
    s.partyRide       = find(facts::kPartyRideActive);
    s.suspended       = find(facts::kRideSuspended);
    s.getOff          = find(facts::kPartyGetOff);
    s.stayLabel       = find(facts::kStayLabel);
    s.stayLabelCall   = site(facts::kStayLabelCall);
    s.accept          = find(facts::kCanAcceptCall);
    s.acceptCall      = site(facts::kCanAcceptCallSite);
    s.soundAsset      = find(facts::kUiSoundAsset);
    s.soundPlay       = find(facts::kUiSoundPlay);
    s.brain           = find(facts::kRequestBrainState);
    s.followCall      = site(facts::kMountAiFollowCall);
    s.findAnim        = find(facts::kFindAnimByNameSlot.target);
    s.rowReturn       = image.Rebase(facts::kWhistleActionCheckReturn, 1).value_or(0);
    s.riderReturn     = image.Rebase(facts::kRiderStopAnimReturn, 1).value_or(0);
    s.icon            = find(facts::kAutoRideIcon);
    s.iconCall        = site(facts::kAutoRideIconCall);
    s.whistleCall     = site(facts::kWhistleOrDismissCall);
    s.fromWhistleCall = site(facts::kCallFromWhistleCall);
    s.getOffCall      = site(facts::kPartyGetOffCall);
    for (std::size_t i = 0; i < kForKeys; ++i)
    {
        s.forKeyCalls[i] = site(facts::kSummonRideForKeyCalls[i]);
    }
    for (std::size_t i = 0; i < kBuggies; ++i)
    {
        s.buggyCalls[i] = site(facts::kSummonCanCallBuggyCalls[i]);
    }
    for (std::size_t i = 0; i < kEntryCalls; ++i)
    {
        s.entryCalls[i] = site(facts::kEntryPartyRideCalls[i]);
    }
    g_sites = s;
    g_image.emplace(image);
    namespace settings = platform::settings;
    g_ownOn.store(settings::Of(settings::kOwnChocobo).word == L"on", std::memory_order_relaxed);
    platform::Log(L"chocobo call: own-chocobo {} ({})", g_ownOn.load(std::memory_order_relaxed) ? L"on" : L"off",
                  settings::Why(settings::kOwnChocobo));
    try
    {
        control::RegisterOption(
            settings::kOwnChocobo,
            [](const std::wstring& word) {
                const bool on = word == L"on";
                if (!on && word != L"off")
                {
                    return control::Outcome{false, L"on or off"};
                }
                g_ownOn.store(on, std::memory_order_relaxed);
                if (!on)
                {
                    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
                    {
                        EndCall(seat, L"own-chocobo turned off");
                        // Turned on again, a ride already under them is theirs, as at a join.
                        const auto index      = static_cast<std::size_t>(seat);
                        g_windowMember[index] = nullptr;
                        g_story[index]        = false;
                        g_leaderSeen[index]   = -1;
                        CloseWindows(seat);
                    }
                }
                platform::Log(L"chocobo call: own-chocobo {} (set): {}", on ? L"on" : L"off",
                              on ? L"players 2 and 3 call, mount and get off their own chocobo"
                                 : L"players 2 and 3 ride along with player 1's party ride");
                return control::Outcome{};
            },
            [] { return std::wstring(g_ownOn.load(std::memory_order_relaxed) ? L"on" : L"off"); });
    }
    catch (...)
    {
        platform::Log(L"chocobo call: the own-chocobo option could not be registered (out of memory?); the page cannot set it");
    }
    // The party getoff's call crosses a 16-byte block: every site is placed from the game thread, between its
    // ticks (platform/deferred_sites.h).
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread() && !placed)
    {
        platform::Log(L"chocobo call: UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();
    RestoreAll();
    g_called.fill(false);
}

bool RunSeatSummon(void (*body)(void* state, float deltaTime), void* state, float deltaTime) noexcept
{
    const int seat = OpenSeatHere();
    if (seat < 1 || !g_ownOn.load(std::memory_order_relaxed) || !g_whistleCall.IsInstalled() || g_summonSeat != 0)
    {
        return false;
    }
    // Cleared again at the next tick too: a fault inside the body leaves through the member tick's guard, past this.
    g_summonSeat = seat;
    g_rowByMod   = false;  // set by this body's row check
    body(state, deltaTime);
    g_summonSeat = 0;
    return true;
}

void EndForForcedGetOff(const wchar_t* what, bool instant) noexcept
{
    if (OpenSeatHere() != 0 || !players::generation::OnGameThread())
    {
        return;
    }
    bool any = std::any_of(g_called.begin(), g_called.end(), [](bool called) { return called; });
    for (int seat = 1; instant && seat <= players::kSeatsBuilt; ++seat)
    {
        any = any || chocobo_ride::Driving(seat);
    }
    if (!any)
    {
        return;
    }
    const std::wstring why = std::wstring(L"the game's forced getoff from ") + what;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (instant && chocobo_ride::Driving(seat))
        {
            // An instant getoff removes the chocobos before the ride's own end would run on the next tick: the
            // game's end of their coordinator runs now, while their chocobo is still there.
            chocobo_ride::Release(seat, why.c_str());
        }
        EndCall(seat, why.c_str());
    }
}

bool OwnCall(int seat) noexcept
{
    return players::SeatBuilt(seat) && g_called[static_cast<std::size_t>(seat)];
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    g_summonSeat = 0;  // set only inside a summon body; a fault there may have left it
    g_notifySeat = 0;
    const std::uint64_t  now      = GetTickCount64();
    static std::uint64_t namesTry = 0;  // the rider's stop pose names: tried once a second until each exists
    if (now - namesTry >= 1000)
    {
        namesTry = now;
        for (std::size_t i = 0; i < kRiderStopNames.size(); ++i)
        {
            if (g_riderAsked[i].load(std::memory_order_relaxed) == 0)
            {
                g_riderAsked[i].store(world::data_rows::FindName(kRiderStopNames[i]), std::memory_order_relaxed);
            }
            if (g_riderFallback[i].load(std::memory_order_relaxed) == 0)
            {
                g_riderFallback[i].store(world::data_rows::FindName(kRiderStopFallbacks[i]), std::memory_order_relaxed);
            }
        }
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto index = static_cast<std::size_t>(seat);
        if (!players::generation::Active(seat))
        {
            g_windowMember[index] = nullptr;  // a ride under them when they join again is theirs
            g_story[index]        = false;
            g_iconSaid[index]     = false;
        }
        if (!g_called[index])
        {
            continue;
        }
        if (!players::generation::Active(seat))
        {
            EndCall(seat, L"they left the game");
            continue;
        }
        if (players::generation::MemberOf(seat) != g_calledMember[index])
        {
            EndCall(seat, L"they play another member now");
            continue;
        }
        const int entry = chocobo_ride::MemberEntryState(players::generation::MemberOf(seat));
        if (entry != g_entrySaid[index])
        {
            g_entrySaid[index] = entry;
            platform::LogDebug(L"chocobo call: {}'s member's entry state {} (0 idle, 1 named, 2 spawning, 3 placed, 4 riding, 5 "
                               L"dismounting; the ride manager's assets {})",
                               players::SeatName(seat), entry,
                               ReadField<std::uint8_t>(chocobo_ride::RideManagerNow(), facts::kRideManagerAssets));
        }
        g_riddenSaid[index] = g_riddenSaid[index] || entry == kEntryRiding;
        if (!g_riddenSaid[index] && now - g_calledAt[index] >= kGiveUpMs)
        {
            EndCall(seat, entry <= 1 ? L"no chocobo came: the ride manager may not have its assets, or their member's ride did "
                                       L"not start"
                                     : L"no chocobo seated their member in time");
        }
        else if (g_riddenSaid[index] && entry != kEntryRiding)
        {
            EndCall(seat, L"their member got off (a fight, an event, a warp or the party's getoff)");
        }
    }
}

}  // namespace lss::rides::chocobo_call
