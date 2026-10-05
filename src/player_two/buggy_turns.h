#pragma once

// The buggy is one vehicle for the whole party: while player 1 rides it, the players take turns at its wheel (the
// user, 2026-10-02: "split screen is disabled for that anyway, because two mirror images would be weird"; plan,
// "Buggy: single view").
//
// The signal is the game's own: player 1's AEndPlayerController::ActiveVehicleCoordinator (`PC+0x5B8`) is the buggy
// coordinator (by its vtable) in any state, from boarding (3) through riding (4) to getting off (5); the game clears it
// after (RE-p2-control-mode-object.md; confirmed live 2026-10-04 by player_two/ride_watch). While it holds, the
// caller (mod.cpp):
//   - has player 2 stand aside (Session::SetSharedRide): player 2's member goes back to its AI, as in the one-player
//     game, and player 2 stays in without one; the 2nd slot's member is taken again once the ride is over;
//   - shows player 1's single view;
//   - stands the per-pad input down (pad_sticks::SetStoodDown), so the game's own merge reaches player 1 from every
//     pad, as in the one-player game.
// Logged: each change of the ride (Info), and where the member player 2 gave back is at each buggy state (Info:
// beside the buggy or left behind), to see what the game's boarding does with it. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::buggy_turns {

/// Finds the buggy coordinator's vtable; logged either way. False if it differs (the buggy is then not seen).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, before the session's: whether player 1 rides the buggy now (false without player 2, or when
/// not installed).
bool Tick() noexcept;

}  // namespace lss::player_two::buggy_turns
