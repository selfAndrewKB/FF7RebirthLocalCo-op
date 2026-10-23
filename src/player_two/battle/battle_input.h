#pragma once

// Player 2's battle state, read and logged inside player 2's member tick scopes, before the tick (a dev tool: Trace,
// on change; dev/dev_tools.h installs it, player_two/input/member_movement.h calls it; a player's build has the stubs
// below).
//
// Nothing here drives player 2's battle: attack, the unique ability, guard and dodge go through player
// 2's own command controller and the game's own handler (player_two/battle/command_menu_tick.h); the soft
// target, lock, lock camera, switching and cursor come from the game's own targeting tick run for
// player 2 (player_two/battle/lock_on.h). The direct calls and the mod's own targeting this module once ran
// (RE-p2-battle-input.md §5.1) were recreations and are retired.
//
// Logged on change: player 1's command controller's gates and requests, both characters' battle preconditions and
// move-state bytes, the target fields. Game thread only.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::battle_input {

#if LSS_DEV

bool Install(const platform::GameImage& image) noexcept;

/// Inside player 2's member tick scopes, before the tick: the battle state, logged on change.
void Watch(void* member, void* state) noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void Watch(void*, void*) noexcept {}

#endif

}  // namespace lss::player_two::battle_input
