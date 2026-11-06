#pragma once

// A further player climbs chocobo walls with their own chocobo (research/RE-p2-chocobo-climb.md, levers L-B and L-C;
// L-A, the registration on the seat's controller, is seat_triggers' "chocobo climb" class).
//
// The climb spot's per-frame try-begin (its vtable slot, called by the character's field-action notify pass for each
// spot on its controller's list) asks the primary controller whether the chocobo is the ridden vehicle and whether its
// member is busy. For the chocobo a seat rides it runs inside the seat's primary scope and leader answer
// (rides::chocobo_ride::RunAsRider), as the game runs player 1's: the state change, the climb executer's Start and its locks
// then happen as the seat's. The executer's state 1 enter sets up the climb camera only when the primary coordinator's
// riders include the party leader; for a seat's chocobo it runs inside the same scope with the seat's member as the
// leader, so the game's request reaches the seat's chocobo's own ride camera module on the seat's camera. Every other
// spot and chocobo runs as the game runs it. The steering (the move state's slot 16, in the seat's ride tick) and the
// climb motion (an extra action on the animation worker) need nothing.
//
// Logged: installs (Info); each start of a seat's (Debug: the game's ridden-vehicle answer in the seat's scope, the
// field-action character of the seat's controller, the chocobo's control state before and after); the camera setup
// for any player (Debug: the module's state before and after); a seat's climb camera's state changes (Debug);
// a 10 s summary (Debug). Game thread.
//
// Per seat (players/seats.h): the chocobo a seat rides names the seat (rides::chocobo_ride::SeatRiding); the start check and
// the camera setup run in that seat's scope with its member as the leader. The counts, the noted start check and the
// watched module are kept per seat.

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::chocobo_climb {

/// Replaces the climb spot's try-begin slot and the climb executer's state 1 enter (each checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, every engine tick: each seat's climb camera's state changes and the summary (Debug).
void Tick() noexcept;

}  // namespace lss::traversal::chocobo_climb
