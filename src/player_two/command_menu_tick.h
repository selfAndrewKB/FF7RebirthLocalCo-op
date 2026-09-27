#pragma once

// Player 2's command controller ticked at the game's own spot, and player 2's battle actions driven
// through it (RE-p2-command-menu-tick.md §13, pass S-CMD3; RE-p2-command-menu.md §5.1).
//
// The game ticks its one command controller once per frame, from the game mode's phase-0 global tick
// (call 0x140C92978). That call is redirected: the game's controller ticks as before, then, in
// battle, player 2's controller (player_two/command_menu_two.h) ticks inside player 2's scopes: its
// focus context and the UI system's controller slot, the primary controller, the party leader
// (weak pointer and type) answering player 2's member, player 2's target fields, and pad 2's sticks
// and buttons. The controller's own sub-ticks then call the game's per-character attack, guard and
// dodge functions on player 2's member, so player 2's Attack, Unique, Guard and Dodge are written
// into its request array from pad 2 (levels, as the game's input handler and up route leave them)
// and the direct calls in player_two/battle_input are retired while this holds.
//
// What a second tick would change for player 1 is shielded:
//   - the world slow (slot 0, written by every tick): either menu slows the world (policy A, the
//     smaller of the two values is written);
//   - the per-type command snapshot and the shared key guide: player 2's pass does not write them;
//   - two shared UI values it writes each tick: saved and restored around it;
//   - the leader-switch requests: cleared before each tick (player 2's d-pad never switches);
//   - the battle HUD mode is mirrored with the controller's own setter, never the global one;
//   - the command rules are turned around for the pass: player 2 may not command player 1's leader.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_menu_tick {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Player 2's attack, guard and dodge go through player 2's command controller (the direct calls
/// in player_two/battle_input must not run as well).
bool DrivesActions() noexcept;

}  // namespace lss::player_two::command_menu_tick
