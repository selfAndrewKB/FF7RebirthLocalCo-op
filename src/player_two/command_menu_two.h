#pragma once

// Player 2's own battle command menu: a second FieldCommandCtrl (RE-p2-command-menu.md §5.1, §10;
// RE-p2-command-menu-input.md §4.1, §10 Stage 3a; passes S-CMD1, S-CMD2).
//
// The game owns one command controller (EndUISystem[23]) and builds it with three plain calls:
// FMemory_Malloc(0x798), FieldCommandCtrl__Ctor, FieldCommandCtrl__SetupMenuWidgets(ctrl, world). The
// ctor registers the controller with the game's GC-referenced list and Setup creates its own widgets,
// states and request arrays. Player 2's controller is built the same way, once, and set up again for
// each world (as MasterHUD_Tick re-runs Setup for the game's). Like the game's, it is never freed.
// Setup makes focus calls (a list selection; a layer push when battle input is live), so it runs
// inside player 2's focus scope (player_two/focus_scope.h) and never touches player 1's focus.
//
// Its menu widget stays at render opacity 0 until it is shown in player 2's pass only. Stage 3a: built
// and logged only; not ticked, opened or fed input yet. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_menu_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick while player 2 plays: builds or sets up player 2's controller for this world, and
/// logs its bytes on change.
void Tick() noexcept;

/// Each engine tick, with or without player 2: player 2's menu widget kept at render opacity 0 (in
/// battle something outside the controller shows every command widget; RE-p2-command-menu-input.md §12).
void KeepHidden() noexcept;

/// Before a pass of the overlay render (0 player 1, 1 player 2): in battle, player 2's pass shows player
/// 2's command menu (at player 1's menu's render opacity) instead of player 1's; player 1's pass shows
/// player 1's only. Out of battle player 2's stays hidden. Render thread = game thread.
void ShowOnly(int player) noexcept;

/// After the frame's passes: player 1's menu back to its own opacity, player 2's hidden.
void Restore() noexcept;

/// Player 2's controller, or null before it is built.
void* Controller() noexcept;

}  // namespace lss::player_two::command_menu_two
