#pragma once

// Each seat's player rides a Wheelie (SB scooter) of their own (research/RE-p2-wheelie-ride.md, RE-wheelie-polish.md).
//
// The game boards by walking: the on-foot move state's field-summon step asks the primary controller's scooter
// coordinator whether the stick points the leader into its marked scooter (its target: the free one nearest the
// leader, picked each tick from the coordinator's own scooter list), and on yes latches a ride that the coordinator's
// next tick begins for the field leader. The mod skips that step for a seat's member (scopes/scope_leaks.h, leak 5),
// so as the game stands a seat's player can neither board nor board player 1.
//
// Each seat's controller gets its own scooter coordinator with the game's creator as soon as a parked scooter is
// registered on the map, and the same walk-in question is asked of it for the seat's member, with the seat's stick
// and camera, inside the seat's scopes and leader. Only player 1's coordinator has a scooter list (each parked scooter
// registers with the primary controller's at level load), so a seat's coordinator marks a scooter by a pick of the
// mod's: where its tick calls the game's target search, the free scooter of player 1's list nearest the seat's member
// becomes its target, written with the game's own weak-pointer assignment; player 1's coordinator keeps the game's
// search. This is the one recreation of the ride, said plainly: the game's search measures from the party leader and
// warps every free scooter it passes that is far from the leader and from its home back home, so it cannot be run from
// a seat's member. The coordinator's own idle tick then computes its marker (the auto-ride icon) against the character
// its field-leader resolve answers: a seat's member for a seat's coordinator (a call redirect; the game's answer for
// player 1's), so the icon follows the seat's member. The icon is one widget per coordinator in the game's marker
// list; each view shows its own player's (hud/field/field_markers.h asks MarkerOwner). Should the search's call or the
// resolve differ, the question is asked of player 1's coordinator about its mark instead: a seat's player then boards
// only near player 1 (logged at the first tick).
//
// On yes, right after the member's tick and in the same scopes, the game's own ride start runs on the seat's
// coordinator (the game's dispatch with its SetOn pair), then the coordinator's begin with the seat's member as the
// rider (TryBeginRide's scripted branch: the game's resolver answers player 1's field leader, read from the party
// data, which no scope changes) and the game's flush, so the boarding enter (the possession swap, the camera module on
// the seat's camera) runs inside the seat's scopes with the mount animation kept.
//
// While a seat rides, the scooter's move state is ticked with the game's per-character tick in the seat's scopes,
// as the field ticker does for the leader's ride body (it never selects a seat's). The seat's own getoff button
// reaches the seat's coordinator natively in that tick. The getoff animation's end swaps back outside every scope:
// the coordinator's vt+0x2F0 is run inside a seat's scopes for that seat's coordinator (a replaced vtable slot). The
// game's forced getoffs (fast travel, events, level commands, interactions, the field step) reach the primary
// controller's ride only: each call is followed by the same call for a seat's coordinator inside the seat's scope
// (call redirects at the callers). A seat's getoff is made instant only when player 1's leader is more than 50 m from
// the scooter: the game's rental-return rule would otherwise warp it home during the dismount.
//
// The mod's parts are the seat's coordinator's mark, the question asked of it for the seat, the rider passed to the
// begin, the scooter's tick, and the re-issued getoffs; every other call is the game's own. Everything is logged. Game
// thread only.
//
// Per seat (players/seats.h): each seat's coordinator, walk-in answer, ride and "stopped for the session" are its own
// (a record per seat, index 0 unused); the seat comes from the controller the member's tick hands in, the coordinator
// the swap back is called for or whose target is searched, the primary controller at the marker's resolve and the
// getoff button, or the mesh owner of a detach notify. A forced getoff is followed for each riding seat in turn, in
// seat order. Two seats may mark one scooter; the first to board takes it and the other finds it held. The install,
// the slots and call redirects are shared.

#include "input/move_state.h"
#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::rides::wheelie_ride {

/// Finds the game's functions and replaces the coordinator's swap-back slot; logged either way. False if a check
/// failed (a seat's player then cannot board).
bool Install(const platform::GameImage& image) noexcept;

/// Puts the swap-back slot and the getoff call redirects back.
void Remove() noexcept;

/// From scope_leaks' skip of the field-summon step for a seat's member (`state`, its on-foot move state), inside the
/// member tick's scopes: the game's ride question for that seat (found from the state's character). Records a yes for
/// AfterMemberTick.
void Ask(void* state) noexcept;

/// From the member tick right after the game's tick, inside its scopes: boards the yes recorded for the seat whose
/// controller `controller` is.
void AfterMemberTick(void* controller, void* member) noexcept;

/// From the member's tick while a seat's player plays `member` with `controller` (the seat's controller: the
/// seat is found from it). True while the scooter's move state was ticked (or is waiting) instead of the member's own.
bool Tick(void* controller, void* member, const input::move_state::EngineTicks& engine) noexcept;

/// `seat`'s coordinator has a ride (boarding, riding or getting off).
bool Riding(int seat) noexcept;

/// Before the session gives `seat`'s member back, withdraws the seat or it leaves: the game's instant getoff for the
/// seat's coordinator inside the seat's scopes, while they still hold. Logged. Does nothing when the seat's player
/// does not ride.
void Release(int seat, const wchar_t* why) noexcept;

/// The member was destroyed under `seat`'s player: the ride is forgotten without calls. Returns the scooter the
/// seat's controller possessed (null if none), so the session clears that controller field too.
void* Forget(int seat) noexcept;

/// The scooter coordinators' markers (the auto-ride icons) as they are now: player 1's coordinator's widget at [0], each
/// seat's own at [seat] (null where there is none), read once for a pass. Empty while the seats mark nothing of
/// their own (every icon is then every view's). Game thread.
struct MarkerWidgets
{
    const void* widget[players::kMaxPlayers] = {};
};
MarkerWidgets MarkerWidgetsNow() noexcept;

/// Whose marker `widget` is by the snapshot: 0 player 1's, a seat's number for its own, -1 no coordinator's. For
/// hud/field/field_markers, which shows each view only its own player's.
int MarkerOwner(const MarkerWidgets& icons, const void* widget) noexcept;

}  // namespace lss::rides::wheelie_ride
