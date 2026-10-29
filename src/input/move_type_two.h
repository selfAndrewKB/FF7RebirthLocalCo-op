#pragma once

// Each seat's walk cap from a walk-only step ends when the step ends, as player 1's does (players/seats.h;
// research/RE-p2-walk-mode.md §7).
//
// Move-type layer 2 (ActionPlayerMoveType) on "the" primary controller caps a character's gait. A behavior action's
// walk-only step (the leader action's type 4, phase 4) sets it to Walk on its phase enter and back to 6 on its phase
// exit, both through the primary setters. For player 2's member the enter runs on the game thread from Begin (inside
// player 2's primary scope: player 2's controller is capped) or on the anim worker from Advance (player 1's is capped),
// and the exit runs mostly on the worker, outside every scope (player 1's is cleared): player 2, or player 1, stayed
// walking. Red XIII's chain-door action has the same split (Begin, End).
//
// The mod tracks the controller each seat's steps capped (the seat comes from the action's character). A cap on
// another player's controller moves to the seat's own (the game's setter, in the seat's scope) while the step is live;
// an exit that cleared another controller than the capped one queues the game's own layer-2 clear on the capped one.
// Queued clears run on the next game tick, a seat's controller's inside that seat's primary scope (never on the worker:
// the scope swaps a field the game thread reads), and are dropped while a live step owns that controller's cap (player
// 1's own step, or a seat's moved cap); a step's owner is published before its enter's write. A cap left on player 1's
// controller when a seat's player leaves is cleared then.
//
// The controllers are named by player number (PlayerNumber: 1 player 1, then the seat's number plus one; 0 another
// controller), and a set of controllers to clear holds one bit per player number (ClearBit).
// Per seat: the controller its step's cap is on, a pending move, the cached controller and the layer watch. Shared:
// the set of controllers to clear and player 1's own step (both any thread, atomics).
//
// Logging: every layer-2 write through the setter (Debug on change of value, caller and controller, Trace per call;
// 1.005, whose setter entry fits one block), each seat's walk-step enters and exits, each move and clear, and each
// seat's controller's layer 2 on change.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::input::move_type_two {

/// A player's number in the walk-cap bookkeeping: 1 for player 1 (seat 0), seat + 1 for a seat. 0 names another
/// controller. Pure.
constexpr int PlayerNumber(int seat) noexcept
{
    return seat + 1;
}

/// The seat a player number names (0 player 1), or -1 for 0 (another controller) or a number past the players. Pure.
constexpr int SeatOfPlayerNumber(int player) noexcept
{
    return player >= 1 && player <= players::kMaxPlayers ? player - 1 : -1;
}

/// The bit of player number `player` in a set of controllers to clear (bit 1 player 1, bit 2 player 2, bit 3 player
/// 3); 0 for another controller. Pure.
constexpr int ClearBit(int player) noexcept
{
    return player >= 1 && player <= players::kMaxPlayers ? 1 << player : 0;
}

void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick: the deferred clear, the cached member and controller, the layer-2 watch.
void Tick() noexcept;

}  // namespace lss::input::move_type_two
