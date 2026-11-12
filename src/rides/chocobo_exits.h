#pragma once

// A seat's chocobo leaves its glide or water jet in the seat's scope, whoever ends it
// (research/RE-chocobo-exits-pads.md).
//
// The glide and the jet are control states of the chocobo's holder. Every way out of them (the landing, a warp with
// a reset, the battle teleport, a landing in water, a cancel, an event's reset of the holder) switches the holder's
// state, and the switch calls the state's own exit: there is no other caller of the exits. The jet's exit clears a
// layer of the primary camera manager's settings and the glide's stops a camera shake on it. For the chocobo a seat's
// player rides that is the seat's camera manager only while the seat's controller is the primary one, which the
// landing has (it ends inside the seat's ride tick) and the forced ends have not: there the jet's layer would be
// cleared on player 1's camera and stay on the seat's. So the two exit slots are replaced: for a seat's chocobo on
// the game thread, the game's exit runs inside the seat's primary scope and leader answer when that scope is not
// already open; every other chocobo's exit, and any exit off the game thread, is the game's call as it is. The two
// enter slots are replaced for the log alone.
//
// Logged at Debug per enter and exit: whose chocobo, the thread, the scope and the caller (a research address). A
// seat's exit whose scope was refused is a WARNING. Game thread, except the pass-through.
//
// Per seat (players/seats.h): the seat comes from the chocobo the state belongs to (rides/chocobo_ride).

namespace lss::platform {
class GameImage;
}

namespace lss::rides::chocobo_exits {

/// Replaces the four state slots (checked; the two exits together or not at all; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

}  // namespace lss::rides::chocobo_exits
