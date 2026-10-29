#pragma once

// The movement census (a dev tool): which characters' move states run, how often, and how the movement gate's
// per-character tests answer for each (input/move_state.h counts into it from both tick slots; the session's
// status line writes it). Every 30th tick of a character of interest (player 2's member, player 1's pawn and leader)
// samples the five gate tests one by one, since the gate itself starts with a global test and its answer would not say
// why a character is refused. A player's build has the empty stubs below.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::input::move_census {

#if LSS_DEV

/// Finds the five gate tests (logged if one differs: the census then counts ticks only).
bool Install(const platform::GameImage& image) noexcept;

/// A move-state tick on the game thread: `viaTickInput` for the base class's direct call, `ownTick` for one the mod
/// made itself (input/move_state.h OwnTick).
void Count(void* state, void* character, bool viaTickInput, bool ownTick) noexcept;

/// Writes one line per character of interest seen since the last call, then starts afresh.
void Log() noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void Count(void*, void*, bool, bool) noexcept {}
inline void Log() noexcept {}

#endif

}  // namespace lss::input::move_census
