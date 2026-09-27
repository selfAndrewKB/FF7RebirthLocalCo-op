#pragma once

// Player 2's battle targeting, run inside player 2's member tick scopes, before the tick.
//
// Attack, the unique ability, Guard and Dodge are not here: player 2's own command controller takes
// them through the game's own handler and requests (player_two/command_menu_tick.h). The direct
// calls this module once made (RE-p2-battle-input.md §5.1) were a recreation and are retired.
//
// Player 2's soft target (RE-p2-targeting.md §5.2, §5.3): the game picks the leader's soft target in
// one leader-only update. With part 8 the update runs for player 2's member, with the party manager's
// leader answering player 2's member and the target system's lock byte player 2's, both put back
// after the call; the target lands in the member's own BattlePlayer component, as the leader's does.
//
// Player 2's lock-on (player_two/lock_on.h) runs next, with the leader answering the member, as the
// game's targeting tick runs its lock after the soft target.
//
// Each part is switched by the battle mask (8 soft target, 32 lock, 64 switching, 128 lock camera,
// 256 cursor; bits 1, 2, 4 and 16 are retired and ignored). All are on at start; the mask switches a
// part off for diagnosis. Every frame it also logs, on change, what it reads (the controller's gates
// and requests, both characters' battle preconditions and move-state bytes, the targets). Game
// thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::battle_input {

bool Install(const platform::GameImage& image) noexcept;

/// Inside player 2's member tick scopes, before the tick: player 2's targeting; reads and logs on change.
void InScopes(void* member, void* state, float deltaTime) noexcept;

/// Diagnosis and staging: which parts act (the bits above). Logged.
void SetMask(unsigned mask) noexcept;

}  // namespace lss::player_two::battle_input
