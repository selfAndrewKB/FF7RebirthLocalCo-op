#pragma once

// Characters near player 2 animate as the game animates them near player 1 (research/RE-p2-anim-optimisation-two-views.md).
//
// Square's animation optimisation decides every skeletal mesh's update rate and pause level from its distance and
// angle to ONE snapshot of player 1's camera, so characters in player 2's view froze whenever player 1 looked away.
// The decision already takes the minimum over a per-actor list of extra {distance, angle} pairs (linked actors share one
// level through it), which the game clears and refills every frame. After the game's refill and before its parallel
// evaluation, each registered actor gets one more pair: its distance and angle to player 2's camera, measured by the
// game's own function. The game's evaluation then runs unchanged on the nearer or better of the two cameras.
// Game thread, while the split is shown; the timing and counts are logged.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::anim_two_views {

/// Replaces the two calls and finds the game's functions (each checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick (before the world tick): the frame count the hooks compare, whether the split is
/// shown, and the Debug summary.
void Tick(bool splitShown) noexcept;

}  // namespace lss::player_two::anim_two_views
