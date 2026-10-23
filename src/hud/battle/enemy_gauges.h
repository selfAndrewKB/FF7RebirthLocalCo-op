#pragma once

// The enemy name and life gauges (EnemyStatus_C) placed in each player's view (Stage B3b;
// RE-p2-enemy-markers.md §7.3, §7.4 and §7.7 A; RE-p2-bar-targeted.md).
//
// The game's enemy-status manager (EndUISystem+0xF8) places each gauge once per tick, projecting through
// the gauge's own player context and the overlay's view rect: player 1's. Its whole tick cannot run
// again (it creates entries, advances timers and runs a decrementing test per entry), but its two
// placement functions are pure apart from the gauge's cached screen point and its viewport position.
// So:
//   - capture: while the game's tick runs, each placement call is recorded with the arguments the
//     game passed (the untargeted function through a few bytes at its second instruction,
//     hud/battle/enemy_gauges_code.h; the targeted one through its one call site), and so are each entry's
//     "forced targeted" answer and three y-step flags, at the one call site that tests them;
//   - player 1's replay (player 1's re-run): each live gauge placed by the same function with the
//     recorded arguments, then given the tick's final y-offset position for the gauges the tick
//     applies it to (the offset's ease is not advanced). It has every input of the tick, so it
//     reproduces the tick's placement;
//   - player 2's replay (player 2's context and view rect set): each gauge classified as targeted by
//     player 2's own displayed target (or its main target), or by the view-independent cases the
//     tick recorded, and placed by the matching function; the targeted one inside a LeaderScope, as
//     its clamp keeps the gauge clear of the leader. The "targeted" ease (a y offset toward -60 that
//     each gauge keeps for its lifetime, p+0x134) is player 2's own: a copy per gauge, swapped in around
//     the game's own step function, and player 1's value put back after.
// The record is emptied after each frame's overlay render. Fading-out gauges are not replayed.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::enemy_gauges {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// With the view's levers set: every gauge the game placed this frame placed again for that view.
void Replay(bool playerTwo) noexcept;

/// After player 2's render: every gauge's size back to player 1's (player 2's stays in place
/// through player 2's updates and render, where the red ability label reads it).
void EndPlayerTwoPass() noexcept;

/// After the overlay render: the record emptied for the next tick.
void EndFrame() noexcept;

}  // namespace lss::hud::enemy_gauges
