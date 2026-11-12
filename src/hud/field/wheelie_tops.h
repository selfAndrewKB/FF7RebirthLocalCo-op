#pragma once

// The Wheelie ride's distance counter in each rider's own view (research/RE-wheelie-polish.md, the mileage TopUI).
//
// Each scooter coordinator's counter is one widget on the root overlay, shown by that coordinator's riding tick; the
// mod's HUD passes draw the overlay in every view, so player 1's counter showed in each view and a seat's own
// (rides/wheelie_ride.h keeps one per seat) would too. Before each view's render the other players' counters get
// render opacity 0 through the game's setter (players/widget_opacity.h) and their own back after, as the point markers
// and the jet gauges do. Logged at Debug on change. Game thread only.
//
// Per seat (players/seats.h): the widgets come from rides/wheelie_ride by player (player 1's coordinator's, each
// seat's own).

namespace lss::hud::wheelie_tops {

/// Hides, for the render pass about to run, the counters that are another player's than the seat (0 player 1's).
/// Restores first.
void ShowOnly(int seat) noexcept;

/// Every counter ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::hud::wheelie_tops
