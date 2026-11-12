#pragma once

// Each player's traversal rumble plays on their own pad (research/RE-p2-zipline-rumble-hook-who.md).
//
// The game keeps one global rumble list (zip line, chocobo, hookshot, ropes, doors, valves, vehicles, swimming: 48 add
// sites). An add carries no owner. Each controller's leader-action component ticks the list once per frame and plays
// its changes on whichever controller is primary at that moment. A seat's component ticks first each frame, inside
// the seat's primary scope, so every effect played on its pad, player 1's chocobo included, and each effect ran half as
// long (two ticks per frame).
//
// Each seat gets a list of its own, made by the game's allocator and constructor at join and taken down the game's
// way at leave (its channels stopped on the seat's controller, then its own destructor). The add function sends an add
// to a seat's list when it is the seat's: on the game thread while the seat is the primary player (the seat's
// scopes), or on a worker inside the seat's zip-line rumble block. The add itself is the game's, under the game's lock.
// The list tick of a seat's component builds and plays the seat's list with the game's own two functions, with the
// seat as the primary player; player 1's component ticks the global list as the game does (never inside a seat's
// primary scope). The zip line's rumble gate answers yes for a seat's member, as it does for player 1's character.
//
// The game's stop by name follows the add (research/RE-p2-rumble-owner-chocobo.md): on the game thread while a seat is the
// primary player, the list's owner names the seat's list for the length of the game's stop, under the game's lock. The
// footsteps of the chocobo a seat's player rides (the chocobo's own virtual, called outside every scope) run inside
// that seat's primary scope, as the game runs the chocobo player 1 rides, so the game's gate asks the seat's
// coordinator and the add goes to the seat's list.
//
// Sources that run outside the seats' scopes name a seat as the owner of their adds and stops while they run for
// the seat's scooter or chocobo (a per-thread owner; research/RE-p2-rumble-owner-chocobo.md): the scooter move state's slot 1
// (hit and loop) and slot 8 (the loop's stop), the sky glide's rumble delegate, the jump box's OnEnded, and the drift
// and brake actions. The last three often run on the anim worker. There the add goes to the seat's list under the game's
// lock. The drift and brake gates ask whether the chocobo's rider is the player, and the leader test answers yes for
// the seat's member on that thread (scopes::leader_answer::ThreadScope). In 1.004 the jump box's add is inlined: a few
// bytes at its list load, under the game's lock, take the seat's list on the thread the invoker named. A stop of a
// seat's on a worker is left (no source found does one; logged).
//
// Shared rides (player 1 drives the buggy or the bronco, a seat's player stands aside): while that seat's component
// has a force-feedback manager of its own (seat_ffb_managers.h), every add and stop on the game's list is copied onto
// the seat's list, so both pads play the ride (the vehicles add and stop by name, on the game thread). The ride's end
// stops on the seat's list whatever copy is left. Without the seat's own manager there is no copy: the shared
// manager's tracks would let the seat's copy play first and skip player 1's.
//
// Not routed: the hook rope and the hookshot (a seat's player cannot start them). PS5 haptics stay single-target.
//
// Units: traversal_rumble.cpp (the add and the stop, the list tick, the zip line, the seats' lists, the footsteps,
// install, removal and tick), traversal_rumble_copy.cpp (the shared ride's copy), traversal_rumble_sources.cpp (the
// sources outside the seats' scopes, placed from a table) and traversal_rumble_shared.h (what they share).
//
// Per seat (players/seats.h): what is described above holds for every seat alike. Each seat has its own list,
// made at its join and taken down at its leave; an add or a stop is the seat's whose source names it or, on the game
// thread, the seat that is the primary player; a component ticks its own seat's list; the zip line and the sources
// find the seat from their character, chocobo or scooter; the owner's list pointer is swapped to one seat's list at a
// time. The shared ride's copy goes to each seat with its own force-feedback manager. 1.004's jump box names a thread
// and that seat's list per landing in flight, one route slot per seat: two chocobos' landings run on
// different animation workers at once (research/RE-seats-step6-camera-rumble.md), each taking its own slot.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::traversal_rumble {

/// Replaces the add's and the stop's entries, the component tick's entry and its list-tick call, and the zip line's two
/// calls (each checked; all or none; logged), then the chocobo's footstep slot and the sources outside the seats' scopes
/// (each optional; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick: for each built seat, makes its list once it is in (and drops it if it went out
/// another way), its member's type for the sources, and the Debug summary.
void Tick() noexcept;

/// Game thread, while `seat` can still be the primary player (before its generation is withdrawn): stops the seat's
/// channels on its controller and destroys its list.
void SeatOut(int seat) noexcept;

/// Game thread, once per engine tick per seat: copy the game's list onto `seat`'s (a shared ride with the seat's own
/// force-feedback manager) or not. Turning it off stops on the seat's list what was copied and not yet stopped.
void CopyPlayerOne(int seat, bool on) noexcept;

/// Names `seat` as the owner of this thread's rumble adds and stops while it lives: a source of the seat's that runs
/// outside the seat's scopes (the grappling gun's hit, the hookshot move's flight). Any thread; an inner scope's seat
/// is the owner until that scope ends, then the outer's again.
class OwnerScope
{
public:
    explicit OwnerScope(int seat) noexcept;
    ~OwnerScope();
    OwnerScope(const OwnerScope&)            = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    int saved_;
};

}  // namespace lss::feedback::traversal_rumble
