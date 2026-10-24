#pragma once

// Move-state ticks: which characters' move states run, and with which pad.
//
// Every party member has a move state (the character's +0x1750), ticked through slot 11 of its
// vtable. The game's field tick runs exactly one of them each frame, the character it selects from
// global state: the party leader. Both slot-11 entries are wrapped to
//   - record, for each seat (players/seats.h), that the game's ticker ran since the seat's last look,
//     with its delta time, and whether it ticked that seat's member itself
//     (player_two/input/member_movement.h reads this);
//   - give player 1's character only player 1's pad while per-pad sticks are on and a seat's player is in;
//   - count each tick into the movement census (player_two/input/move_census.h, a dev tool).
// A tick the mod makes itself (OwnTick) is counted, never recorded as the game's.
// Per seat: the recorded ticks (one record each, so every seat's member tick sees the frame's). Game thread.

#include "player_two/stage_counters.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::move_state {

/// Replaces both move-state tick slots (each independently). False (logged) if neither could be.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// The game's own move-state ticks since `seat`'s last call (a built seat; game thread).
struct EngineTicks
{
    std::uint32_t count        = 0;
    float         deltaTime    = 0.0f;   // the last one's
    bool          memberTicked = false;  // one of them was the seat's member
    // The game's field ticker ran player 1's ridden chocobo or scooter instead (its move state is not counted above, so
    // a frame where player 1 rides has count 0); with that tick's delta time. A seat's ride
    // (player_two/rides/chocobo_ride.h, player_two/rides/wheelie_ride.h) ticks on it.
    bool          rideTicked   = false;
    float         rideDeltaTime = 0.0f;
};
EngineTicks TakeEngineTicks(int seat) noexcept;

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
