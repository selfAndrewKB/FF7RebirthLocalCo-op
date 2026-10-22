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
// into its request array from pad 2 (levels, as the game's input handler and up route leave them).
// This is the only path for player 2's actions; if it cannot run, the log says why.
//
// What a second tick would change for player 1 is handled:
//   - the world slow (slot 0, written by every tick): either menu slows the world (policy A, the
//     smaller of the two values is written);
//   - the per-type command snapshot: player 2's pass does not write it (its one reader cancels a
//     pending command on the game's controller); the key guide is player 2's own inside the scope;
//   - the leader-switch requests: cleared before each tick (a written exception: player 2's d-pad
//     never switches player 1's leader);
//   - the battle HUD mode, battle input and party-list input follow player 1's through the game's
//     own setters on player 2's controller, never the global ones;
//   - the command rules are turned around for the pass: player 2 may not command player 1's leader.
// Pad 2's presses reach player 2's menu in battle; a release follows its press (the capture map's rule).
// Game thread only.
//
// Units: this file's tick, mirrors and shields (command_menu_tick.cpp); pad 2's presses and releases
// (menu_input_route); the tactical tint and the wait sound (menu_tint); what they share, the pass in flight
// above all (command_menu_tick_shared.h).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_menu_tick {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The pass in flight: 0 none, 1 the game's controller's tick, 2 player 2's controller's, 3 pad 2's press or
/// release in player 2's UI. Game thread.
int Pass() noexcept;

/// The game's own calls that give the controller's command menu the UI focus of the focus context that is
/// open (call it inside player 2's): FieldCommandCtrl_SetMenuFocusLayer783(controller, 0) while its layer
/// byte (+0x783) still says the menu layer is up, then FieldCommandCtrl_ResumeHudInput(controller, 0),
/// which turns battle input on and puts the layer back (its layer setter returns early while the byte is
/// set). False if the stage is not installed or a call faulted.
bool RefocusMenu(void* controller) noexcept;

/// The controller's battle input flag (+0x25C) is on.
bool BattleInputOn(const void* controller) noexcept;

}  // namespace lss::player_two::command_menu_tick
