#pragma once

// Player 2's battle state, read and logged inside player 2's member tick scopes, before the tick.
//
// Nothing here drives player 2's battle: attack, the unique ability, guard and dodge go through player
// 2's own command controller and the game's own handler (player_two/command_menu_tick.h); the soft
// target, lock, lock camera, switching and cursor come from the game's own targeting tick run for
// player 2 (player_two/lock_on.h). The direct calls and the mod's own targeting this module once ran
// (RE-p2-battle-input.md §5.1) were recreations and are retired.
//
// Every frame it logs, on change, what it reads: player 1's command controller's gates and requests,
// both characters' battle preconditions and move-state bytes, the target fields. It also runs the
// per-frame watchers that belong inside player 2's member tick (both camera managers' operators, the
// command rules, the member's camera overrides, the end of player 2's lock after a battle).
//
// The battle mask (`battle-mask`): bit 8 switches player 2's targeting run on or off for diagnosis;
// the other bits are retired and ignored. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::battle_input {

bool Install(const platform::GameImage& image) noexcept;

/// Inside player 2's member tick scopes, before the tick: the watchers; reads and logs on change.
void InScopes(void* member, void* state, float deltaTime) noexcept;

/// Diagnosis: bit 8 is player 2's targeting run. Logged.
void SetMask(unsigned mask) noexcept;

}  // namespace lss::player_two::battle_input
