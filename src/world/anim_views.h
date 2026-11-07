#pragma once

// Characters near a seat's player animate as the game animates them near player 1
// (research/RE-p2-anim-optimisation-two-views.md).
//
// Square's animation optimisation decides every skeletal mesh's update rate and pause level from its distance and
// angle to ONE snapshot of player 1's camera, so characters a seat's player sees froze whenever player 1 looked away.
// The decision already takes the minimum over a per-actor list of extra {distance, angle} pairs (linked actors share one
// level through it), which the game clears and refills every frame. After the game's refill and before its parallel
// evaluation, each registered actor gets one more pair: its distance and angle to a seat's camera, measured
// by the game's own function. The game's evaluation then runs unchanged on the nearest or best of the players' cameras.
// Game thread, while the split is shown; the timing and counts are logged.
//
// The same cameras at tick time (research/RE-p2-detail-drop-p1-facing.md §2): each budgeted mesh's own tick first
// re-measures its distance and angle from player 1's snapshot alone, and SE's ShouldUpdateTransform (mode 1, set by the
// evaluation) then reads that angle to decide whether the mesh refreshes its bones and its KBD component (KineDriver and
// Bonamik bone physics) runs; the KBD tick reads it again later. A seat's chocobo and its rider lost that
// detail whenever player 1 looked away. The game's measure is redirected: when a game caller measures from player 1's
// snapshot, it measures again from a seat's camera and keeps the minimum of each value (the semantics of the
// pair list). Logged for a seat's character and mount: the mode and whether player 1's camera alone would
// have skipped the bones (Debug on change).
//
// Per seat (players/seats.h): the above holds for every active seat's camera. Each actor near a seat's camera gets
// one pair per seat, appended in seat order (the evaluation walks the whole list, which grows by the game's own grow);
// each seat's camera is published for the workers under its own seqlock, and the tick-time measure runs once more per
// seat, keeping the minimum. Shared: the refill and evaluation calls, the counts and the watched meshes (Debug).

namespace lss::platform {
class GameImage;
}

namespace lss::world::anim_views {

/// Replaces the two calls and finds the game's functions (each checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick (before the world tick): the frame count the hooks compare, whether the split is
/// shown, and the Debug summary.
void Tick(bool splitShown) noexcept;

}  // namespace lss::world::anim_views
