#pragma once

// The enemy name and life bars (EnemyStatus_C) placed in each player's view (Stage B3b;
// RE-p2-enemy-markers.md §7.3, §7.4 and §7.7 A).
//
// The game's enemy-status manager (EndUISystem+0xF8) places each bar once per tick, projecting through
// the bar's own player context and the overlay's view rect: player 1's. Its whole tick cannot run
// again (it creates entries, advances timers and runs a decrementing test per entry), but its two
// placement functions are pure apart from the bar's cached screen point and its viewport position.
// So:
//   - capture: while the game's tick runs, each placement call is recorded with the arguments the
//     game passed (the untargeted function through a few bytes at its second instruction,
//     hud/enemy_bars_code.h; the targeted one through its one call site);
//   - replay: in player 2's overlay pass (player 2's context and view rect set) and again in player 1's
//     re-run, each live bar is placed by the same function with the recorded arguments, then given
//     the tick's final y-offset step for the bars the tick applies it to (UUserWidget::
//     SetPositionInViewport with the bar's own values; the offset's lerp is not advanced).
// Player 1's replay has every input of the tick, so it reproduces the tick's placement. The record
// is emptied after each frame's overlay render. Fading-out bars are not replayed.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::enemy_bars {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// With the view's levers set: every bar the game placed this frame placed again.
void Replay() noexcept;

/// After the overlay render: the record emptied for the next tick.
void EndFrame() noexcept;

}  // namespace lss::hud::enemy_bars
