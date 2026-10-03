#pragma once

// Read-only logging for the three gaps of player 2's own chocobo ride (plan §3, live run 2026-10-02): no full sprint
// (RE-p2-chocobo-sprint.md §5), the homing toward player 1 at the ride's start (RE-p2-chocobo-homing.md §6), and the
// navmesh warp while mounted (RE-p2-chocobo-warp.md; its refusal counts are player_two/nav_strand's).
//
// - Sprint: both players' chocobos (gait, speed, ratio, applied root-motion level, the anim instance's walk type and
//   every key of its walk-type map, its speed scale) and both controllers' move-type layers (move, dash, flags,
//   count) and the game mode byte, logged when they change, at most four times a second.
// - Homing: player 2's chocobo's AI (pause mask, pawn, weak pawn, riding latch, brain and state, follow target),
//   its AI move component (status, stored velocity), the chocobo's control holder (the move request), pad 2's own
//   stick, its speed and distance to player 1's, ten times a second for 8 s from the boarding, then on change. The
//   boarding itself is sampled before and after the game's BeginRide.
//
// Reads only; calls nothing of the game's except the weak-pointer resolve. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::ride_probe {

bool Install(const platform::GameImage& image) noexcept;

/// From player_two/chocobo_ride around the game's boarding of player 2's chocobo: `step` names the moment.
void Boarding(const wchar_t* step, const void* chocobo) noexcept;

/// From player_two/member_movement's TickRide, inside every scope, right before (`after` false) and after the game's
/// tick of player 2's chocobo's move state `state`: the move state's input memory (RE-p2-chocobo-input.md §6) and
/// the Accel/Dash held bytes as the tick reads them, logged when they change.
void Slot11(const void* state, bool after) noexcept;

/// Each engine tick, after the member's tick.
void Tick() noexcept;

}  // namespace lss::player_two::ride_probe
