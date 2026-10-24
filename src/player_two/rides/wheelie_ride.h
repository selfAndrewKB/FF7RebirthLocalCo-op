#pragma once

// Player 2 rides a Wheelie (SB scooter) of their own (plan §3, "Wheelie"; RE-p2-wheelie-ride.md §13, §14, option A).
//
// The game boards by walking: the on-foot move state's field-summon step asks the primary controller's scooter
// coordinator whether the stick points the leader into its marked scooter (the free one nearest the leader), and on
// yes latches a ride that the coordinator's next tick begins for the field leader. The mod skips that step for player
// 2's member (player_two/scopes/scope_leaks.h, leak 5), so as the game stands player 2 can neither board nor board
// player 1.
// This asks the same question for player 2's member, with player 2's stick and camera, of player 1's coordinator (the
// only one with a scooter list, so the only marked scooter; option A), inside player 2's scopes and leader. On yes,
// right after the member's tick and in the same scopes, the game's own ride start runs on player 2's controller's own
// scooter coordinator (the game's creator, the game's dispatch with its SetOn pair), then the coordinator's begin with
// player 2's member as the rider (TryBeginRide's scripted branch: the game's resolver answers player 1's field leader,
// read from the party data, which no scope changes) and the game's flush, so the boarding enter (the possession swap,
// the camera module on player 2's camera) runs inside player 2's scopes with the mount animation kept (§14.0).
//
// While player 2 rides, the scooter's move state is ticked with the game's per-character tick in player 2's scopes,
// as the field ticker does for the leader's ride body (it never selects player 2's). Player 2's own getoff button
// reaches player 2's coordinator natively in that tick. The getoff animation's end swaps back outside every scope:
// the coordinator's vt+0x2F0 is run inside player 2's scopes for player 2's coordinator (a replaced vtable slot). The
// game's forced getoffs (fast travel, events, level commands, interactions, the field step) reach the primary
// controller's ride only: each call is followed by the same call for player 2's coordinator inside player 2's scope
// (call redirects at the callers). Player 2's getoff is made instant only when player 1's leader is more than 50 m from
// the scooter: the game's rental-return rule would otherwise warp it home during the dismount (§13.2 row 6).
//
// The mod's parts are the question asked of player 1's coordinator for player 2, the rider passed to the begin, the
// scooter's tick, and the re-issued getoffs; every call is the game's own. Everything is logged. Game thread only.
//
// Per seat (players/seats.h): each seat's walk-in answer, ride and "stopped for the session" are its own (a record per
// seat, index 0 unused); the seat comes from the controller the member's tick hands in, the coordinator the swap back
// is called for, the mesh owner of a detach notify, or the primary controller at the getoff button. A forced getoff is
// followed for each riding seat in turn, in seat order. The walk-in question itself (Ask) stays player 2's: whether a
// further seat may ask player 1's coordinator about its one marked scooter is open. The install, the slots and call
// redirects are shared. The names without a seat at the end are player 2's, for the callers not yet per seat.

#include "player_two/input/move_state.h"
#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::wheelie_ride {

/// Finds the game's functions and replaces the coordinator's swap-back slot; logged either way. False if a check
/// failed (player 2 then cannot board, as before).
bool Install(const platform::GameImage& image) noexcept;

/// Puts the swap-back slot and the getoff call redirects back.
void Remove() noexcept;

/// From scope_leaks' skip of the field-summon step for player 2's member (`state`, its on-foot move state), inside
/// the member tick's scopes: the game's ride question for player 2 (player 2's only: the header's open question).
/// Records a yes for AfterMemberTick.
void Ask(void* state) noexcept;

/// From the member tick right after the game's tick, inside its scopes: boards the yes recorded for the seat whose
/// controller `playerTwoController` is.
void AfterMemberTick(void* playerTwoController, void* member) noexcept;

/// From the member's tick while a seat's player plays `member` with `playerTwoController` (the seat's controller: the
/// seat is found from it). True while the scooter's move state was ticked (or is waiting) instead of the member's own.
bool Tick(void* playerTwoController, void* member, const move_state::EngineTicks& engine) noexcept;

/// `seat`'s coordinator has a ride (boarding, riding or getting off).
bool Riding(int seat) noexcept;

/// Before the session gives `seat`'s member back, withdraws the seat or it leaves: the game's instant getoff for the
/// seat's coordinator inside the seat's scopes, while they still hold. Logged. Does nothing when the seat's player
/// does not ride.
void Release(int seat, const wchar_t* why) noexcept;

/// The member was destroyed under `seat`'s player: the ride is forgotten without calls. Returns the scooter the
/// seat's controller possessed (null if none), so the session clears that controller field too.
void* Forget(int seat) noexcept;

// Player 2's, for the callers not yet per seat.
inline bool Riding() noexcept
{
    return Riding(players::kPlayerTwoSeat);
}

}  // namespace lss::player_two::wheelie_ride
