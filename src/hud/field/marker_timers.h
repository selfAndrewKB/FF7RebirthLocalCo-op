#pragma once

// Each view's own linger timer for the off-screen enemy arrows (OutOfScreenTarget_C;
// RE-p2-enemy-markers.md §7.5 and §7.7 B).
//
// The arrows belong to the marker list (EndUISystem+0x150), which player 2's pass already runs again
// with player 2's levers (hud/passes/player_two_pass.h). One function places each arrow from its view, and
// its only state is a per-element float T (element +0x14): set to full while the enemy is off-screen,
// run down by the HUD time step while it is on-screen, and while T is full the arrow stays pinned to
// the screen edge. One T shared by two views, with a time step of 0 in the re-runs, pinned player 1's
// off-screen arrows to the edge of player 2's half (and back).
//
// So each view keeps its own T per arrow (keyed by the element's weak widget): around player 2's run
// of the list, player 1's T values are set aside and player 2's put in (0 for an arrow player 2 has
// not seen), and that run uses the real time step; after it, player 2's are kept and player 1's put
// back. Player 1's re-run is left as it is (its own T, a time step of 0), which gives the tick's
// result. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::marker_timers {

bool Install(const platform::GameImage& image) noexcept;

/// Before player 2's run of the marker list.
void SwapIn() noexcept;

/// After player 2's run of the marker list.
void SwapOut() noexcept;

/// Player 2 is out: player 2's timers forgotten.
void Forget() noexcept;

}  // namespace lss::hud::marker_timers
