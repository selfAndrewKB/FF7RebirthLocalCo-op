#pragma once

// Yuffie's breakable boxes for players 2 and 3: a box counts as near by their own distance
// (research/RE-yuffie-box-throw.md). The game keeps one "the party leader is near" byte per box, refreshed once per
// frame from the party leader's position, outside every scope the mod holds, and the throw selection takes only boxes
// with it set: so a box was a candidate for a seat's player exactly while player 1 stood within its radius. This runs
// the game's own refresh with the seat's member as the party leader right before the seat's member tick (where their
// selection runs), and again for player 1 after it, so each player's selection reads their own distances. One byte
// per box, so the two cannot hold at once; the swap around the tick is the game's semantics asked per player. Game
// thread.

namespace lss::platform {
class GameImage;
}

namespace lss::world::shooting_targets {

/// Finds the game's refresh and the global it reads (checked; logged). False (the boxes answer for player 1 alone, as
/// the game has it) if either differs.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Every box's leader-near byte answered for `member` (a seat's): the game's refresh inside the party-leader scope.
void RefreshFor(void* member) noexcept;

/// The same for the party leader (player 1), outside any scope.
void RefreshForPlayerOne() noexcept;

}  // namespace lss::world::shooting_targets
