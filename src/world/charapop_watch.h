#pragma once

// Logs, read-only, how the game's character detail treats the characters near the seats' players
// (RE-p2-charapop-per-view.md §3; plan §3 item 7). CharaPop decides each NPC's tick pause, its hide
// with a fade and its detail level from one camera, player 1's; SE's animation optimisation decides
// shadows, update rate and anim pauses from a camera snapshot of player 1's too.
//
// While the split is on screen, every 10 ticks: the CharaPop parameters and override distances, the
// tick's counters, the animation snapshot's camera and every player's camera (on change, and every
// 60 s); then, of the entries more than 50 m from player 1's camera or outside its 60° anim cone, the
// 16 nearest to a seat's camera, each with its distance and angle to each camera, its level, minimum level,
// state, pause flags, and its mesh's animation-optimisation fields and the actor's own paused state
// (the interface query the evaluators compare with), logged on first sight and on a change that held
// 3 samples.
// Calls only the game's weak-pointer assign and
// resolve, guarded. Changes nothing. Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::charapop_watch {

/// Finds the globals; logged either way. False if a check failed (nothing is sampled then).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether the split is on screen.
void Tick(bool splitShown) noexcept;

}  // namespace lss::world::charapop_watch
