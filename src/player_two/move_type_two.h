#pragma once

// Player 2's walk cap from a walk-only step ends when the step ends, as player 1's does (research/RE-p2-walk-mode.md §7).
//
// Move-type layer 2 (ActionPlayerMoveType) on "the" primary controller caps a character's gait. A behavior action's
// walk-only step (the leader action's type 4, phase 4) sets it to Walk on its phase enter and back to 6 on its phase
// exit, both through the primary setters. For player 2's member the enter runs on the game thread from Begin (inside
// player 2's primary scope: player 2's controller is capped) or on the anim worker from Advance (player 1's is capped),
// and the exit runs mostly on the worker, outside every scope (player 1's is cleared): player 2, or player 1, stayed
// walking. Red XIII's chain-door action has the same split (Begin, End).
//
// The mod tracks the controller each of player 2's steps capped. A cap on player 1's controller moves to player 2's
// (the game's setter, in player 2's scope) while the step is live; an exit that cleared another controller than the
// capped one queues the game's own layer-2 clear on the capped one. Queued clears run on the next game tick, player 2's
// inside player 2's primary scope (never on the worker: the scope swaps a field the game thread reads), and are dropped
// while a live step owns that controller's cap (player 1's own step, or player 2's moved cap); a step's owner is
// published before its enter's write. A cap left on player 1's controller when player 2 leaves is cleared then.
//
// Logging: every layer-2 write through the setter (Debug on change of value, caller and controller, Trace per call;
// 1.005, whose setter entry fits one block), player 2's walk-step enters and exits, each move and clear, and player 2's
// controller's layer 2 on change.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::move_type_two {

void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick: the deferred clear, the cached member and controller, the layer-2 watch.
void Tick() noexcept;

}  // namespace lss::player_two::move_type_two
