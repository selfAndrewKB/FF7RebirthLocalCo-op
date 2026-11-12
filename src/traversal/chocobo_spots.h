#pragma once

// A seat's chocobo launches from the launch pads, jumps the jump boxes and takes the sliders with its own rider
// (research/RE-chocobo-exits-pads.md, the climb's shape; the registration on the seat's controller is
// world/seat_triggers' three chocobo spot classes).
//
// Each spot's per-frame try-begin (its vtable slot, called by the character's field-action notify pass for each spot
// on its controller's list) asks the primary controller whether the chocobo is the ridden vehicle and whether its
// member is busy, and the launch pad's reads the held Accel from the merged pads. For the chocobo a seat rides they run
// inside the seat's primary scope and leader answer (rides/chocobo_ride), the pad's with the seat's pad's buttons too,
// as the game runs player 1's: the glide starts, or the field action (state 8) with its executer, its locks and its
// camera request on the chocobo's own module, all as the seat's. The jump box's per-frame update decides its combo
// jump through the primary controller's coordinator: for a seat's chocobo it runs in the same scope. Every other
// spot and chocobo runs as the game runs it.
//
// Logged: the install (Info); each start of a seat's (Debug: the chocobo's control state before and after); a 10 s
// summary (Debug). Game thread.
//
// Per seat (players/seats.h): the chocobo a seat rides names the seat; the counts are kept per seat and spot.

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::chocobo_spots {

/// Replaces the three try-begin slots and the jump box's update slot (each checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, every engine tick: the summary (Debug).
void Tick() noexcept;

}  // namespace lss::traversal::chocobo_spots
