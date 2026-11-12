#pragma once

// Each view's own linger timer for the off-screen enemy arrows (OutOfScreenTarget_C;
// research/RE-p2-enemy-markers.md).
//
// The arrows belong to the marker list (EndUISystem+0x150), which each seat's pass already runs again
// with the seat's levers (hud/passes/seat_hud_content.h). One function places each arrow from its view, and
// its only state is a per-element float T (element +0x14): set to full while the enemy is off-screen,
// run down by the HUD time step while it is on-screen, and while T is full the arrow stays pinned to
// the screen edge. One T shared by two views, with a time step of 0 in the re-runs, pinned player 1's
// off-screen arrows to the edge of a seat's half (and back).
//
// So each view keeps its own T per arrow (keyed by the element's weak widget): around a seat's run
// of the list, player 1's T values are set aside and the seat's put in (0 for an arrow the seat has
// not seen), and that run uses the real time step; after it, the seat's are kept and player 1's put
// back. Player 1's re-run is left as it is (its own T, a time step of 0), which gives the tick's
// result. One set of timers per seat, and one per column for a mirror's pass there (a mirror's view has another
// shape than player 1's, so an enemy can be off-screen in it alone): a mirror's set is forgotten in a frame with no
// mirror pass in its column. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::marker_timers {

bool Install(const platform::GameImage& image) noexcept;

/// Before a further column's run of the marker list: `seat`'s set, or (seat 0) the mirror's in `column`.
void SwapIn(int seat, int column) noexcept;

/// After that run of the marker list.
void SwapOut(int seat, int column) noexcept;

/// After the frame's overlay render: a mirror's set whose column had no mirror pass this frame forgotten.
void EndFrame() noexcept;

/// Every seat is out: every seat's timers forgotten (a mirror's set lives while its column has a mirror pass).
void Forget() noexcept;

/// The seat is out while another stays: its timers forgotten (its next run starts each arrow at 0).
void Forget(int seat) noexcept;

}  // namespace lss::hud::marker_timers
