#pragma once

// The chocobo's water-jet gauge in each view: a seat's chocobo's gauge placed from the seat's view and shown there only
// (research/RE-chocobo-exits-pads.md).
//
// Each chocobo's jet gauge component makes one widget on the root overlay and places it every tick at the socket above
// the chocobo, projected through "the first local player" (the world's controller list: player 1's), so a seat's
// gauge sat at player 1's screen spot in every view. Two levers, both the game's own:
// - the component's BeginPlay (a replaced slot) notes each gauge component by its chocobo;
// - in a seat's HUD pass the game's own placement runs again for the seat's ridden chocobo's component with the seat's
//   controller first in the world's list (players/first_player_scope.h), so the projection is the seat's view's;
//   player 1's gauge keeps the game's own placement.
// Before each view's render the other players' gauge widgets get render opacity 0 through the game's setter
// (players/widget_opacity.h) and their own back after, as the point markers do. Logged at Debug on change. Game thread
// only.
//
// Per seat (players/seats.h): the chocobo a seat drives names its gauge (rides/chocobo_ride); player 1's is the
// chocobo player 1 possesses.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::jet_gauges {

/// Finds the placement and replaces the component's BeginPlay slot (checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// In `seat`'s HUD pass: the seat's chocobo's gauge placed again from the seat's view. Nothing for player 1 or a
/// mirror's column.
void Place(int seat) noexcept;

/// Hides, for the render pass about to run, the gauges that are another player's than the seat (0 player 1's).
/// Restores first.
void ShowOnly(int seat) noexcept;

/// Every gauge ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::hud::jet_gauges
