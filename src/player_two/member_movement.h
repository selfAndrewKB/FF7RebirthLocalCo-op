#pragma once

// Moving player 2's party member with player 2's pad.
//
// The game's field tick runs one move state a frame, the party leader's, so the member player 2
// possesses never reads a stick. Once per frame, after the game's ticker has run, this ticks the
// member's move state with the game's own per-character tick, inside three scopes:
//   - player 2's pad as the merged stick (player_two/pad_sticks.h);
//   - player 2 as the primary player, so the tick takes its direction from player 2's camera
//     (player_two/primary_controller.h);
//   - a yes from the movement gate's party-leader test for the member's player type only
//     (a replaced vtable slot that answers on the game thread, inside this tick, and nowhere else).
// It runs only while per-pad sticks are on.
//
// Game thread only.

#include "player_two/stage_counters.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::member_movement {

/// Finds the tick and replaces the leader-test slot. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Once per game tick while player 2 plays `member` with `playerTwoController`.
void Tick(void* playerTwoController, void* member) noexcept;

/// acted: the member was ticked with every scope held; refused: not ticked because a scope could
/// not be held or a precondition failed; passed: not ticked because the game's ticker did not run
/// or ticked the member itself. If the engine raises an exception inside the tick, the tick stops
/// for the rest of the session (logged).
StageCounters& Counters() noexcept;

/// Times the leader test answered yes for the member since the last call.
unsigned TakeLeaderAnswers() noexcept;

}  // namespace lss::player_two::member_movement
