#pragma once

// Move-state ticks: which characters' move states run, and with which pad.
//
// Every party member has a move state (the character's +0x1750), ticked through slot 11 of its
// vtable. The game's field tick runs exactly one of them each frame, the character it selects from
// global state: the party leader. Both slot-11 entries are wrapped to
//   - count, per character of interest, ticks and the movement gate's per-character tests
//     (logged on the status line);
//   - record that the game's ticker ran since the last look, with its delta time, and whether it
//     ticked player 2's member itself (player_two/member_movement.h reads this);
//   - give player 1's character only player 1's pad while per-pad sticks are on.
// A tick the mod makes itself (OwnTick) is counted, never recorded as the game's.

#include "player_two/stage_counters.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::move_state {

/// Replaces both move-state tick slots (each independently). False (logged) if neither could be.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Writes one line per character of interest seen since the last call, then starts afresh.
void LogCensus() noexcept;

/// The game's own move-state ticks since the last call (game thread).
struct EngineTicks
{
    std::uint32_t count        = 0;
    float         deltaTime    = 0.0f;   // the last one's
    bool          memberTicked = false;  // one of them was player 2's member
    // The game's field ticker ran player 1's ridden chocobo or scooter instead (its move state is not counted above, so
    // a frame where player 1 rides has count 0); with that tick's delta time. Player 2's ride (player_two/chocobo_ride.h,
    // player_two/wheelie_ride.h) ticks on it.
    bool          rideTicked   = false;
    float         rideDeltaTime = 0.0f;
};
EngineTicks TakeEngineTicks() noexcept;

/// Marks the move-state ticks made while it lives as the mod's own (game thread).
class OwnTick
{
public:
    OwnTick() noexcept;
    ~OwnTick();
    OwnTick(const OwnTick&)            = delete;
    OwnTick& operator=(const OwnTick&) = delete;
};

}  // namespace lss::player_two::move_state
