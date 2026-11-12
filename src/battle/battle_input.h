#pragma once

// A seat's battle state, read and logged inside the seat's member tick scopes, before the tick (a dev tool: Trace,
// on change; dev/dev_tools.h installs it, input/member_movement.h calls it; a player's build has the stubs
// below).
//
// Nothing here drives a seat's battle: attack, the unique ability, guard and dodge go through the seat's
// own command controller and the game's own handler (battle/command_menu_tick.h); the soft
// target, lock, lock camera, switching and cursor come from the game's own targeting tick run for
// the seat (battle/lock_on.h). None of that is done by the mod itself: direct calls or a targeting of the mod's own
// would be recreations (research/RE-p2-battle-input.md).
//
// Logged on change: player 1's command controller's gates and requests, both characters' battle preconditions and
// move-state bytes, the target fields. Game thread only.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_input {

#if LSS_DEV

bool Install(const platform::GameImage& image) noexcept;

/// Inside `seat`'s member tick scopes, before the tick: the battle state, logged on change.
void Watch(int seat, void* member, void* state) noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void Watch(int, void*, void*) noexcept {}

#endif

}  // namespace lss::battle::battle_input
