#pragma once

// Player 2's walk and run limits come from player 2's own surroundings (RE-p2-walk-mode.md §4.2, §4.3).
//
// A character's speed is capped by move-type layers on "the" primary player controller. Two of them depend on
// where the character stands:
//
// - Layer 8 (InDoor rooms) is written by the controller's UEndPlayerExistNavAreaComponent, which the game creates
//   for local player 0's controller only. Player 2's controller gets one from the game's own creation function, run
//   once inside primary_controller::Scope; the component's tick already runs as player 2's
//   (player_two/location_rows.h), so the game's InDoor rooms then reach player 2's controller and camera.
// - Layer 7 (location walk zones such as a town's) is decided by the global location manager for one character, the
//   party leader, and written through its listener to the primary controller. When player 2 takes a member, the
//   game's party switch copies the leader's layer 7 onto player 2's controller, and nothing ever clears it there.
//   The manager has no per-player test and also drives BGM, names and flags, so it cannot run for player 2; this is
//   a documented recreation of its bookkeeping only (plan, RE-p2-walk-mode.md §4.3). Each tick the mod tests player
//   2's member against the game's move-type volumes with the game's own containment test and rules (strictly inside
//   to enter, the volume's UntouchMargin to stay, bForce and bDisabled honoured, leaves before enters), keeps the
//   game's refcounted move-type stack for player 2, and when player 2's controller's layer 7 differs from the stack's
//   top, calls the game's own listener method inside primary_controller::Scope to write it.
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::walk_limits {

bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the session's tick (a take's copy is overwritten in the same frame).
void Tick() noexcept;

}  // namespace lss::player_two::walk_limits
