#pragma once

// Logging only (development build): why a player's "Terrain Action" row lapses while the other
// player's member stands at the same spot (RE-p2-terrain-action-conflict.md §6).
//
// Once per frame while player 2 is in, before the session's own tick, it reads for each player the
// terrain detector at their leader-action component +0xEE0 (the result bytes of the last evaluation:
// ledge found, landing space failed, request block reached...; the target position), the row-11
// request byte, the global prohibit counters the request block tests, and both pawns' locations.
// A line is written whenever any byte or counter changes (positions are carried, not compared).
// Nothing is written to the game. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::dev::terrain_watch {

void Install(const platform::GameImage& image) noexcept;
void Tick() noexcept;

}  // namespace lss::dev::terrain_watch
