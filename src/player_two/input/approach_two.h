#pragma once

// Player 2 starts ladders and the other walk-up traversals from any side, as player 1 does
// (research/RE-p2-ladder-approach-L2.md).
//
// A traversal actor's notify starts the move when the character is near it and pushes the stick toward it. Square's
// approach check reads the merged stick (the pad that pushes hardest) and turns it into the world by the yaw of the
// primary camera manager, i.e. player 1's camera. Player 2 aims in their own camera's basis, so the check only passed
// while both cameras faced within about 45 degrees of each other. At each notify's call of the check, the check runs
// for player 2's member with pad 2's stick and player 2 as the primary player (the camera manager getter answers player
// 2's, the input lock is player 2's), and for player 1's character with pad 1's stick only. The engage itself runs
// as before. Game thread; logged.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::approach_two {

/// Replaces the notifies' calls of the check (each checked; logged). Calls that cross a 16-byte line are only found
/// here and swapped by Tick.
void Install(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick outside the world tick: swaps the crossing calls the first time (logged).
void Tick() noexcept;

void Remove() noexcept;

}  // namespace lss::player_two::approach_two
