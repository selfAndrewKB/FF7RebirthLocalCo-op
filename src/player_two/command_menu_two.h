#pragma once

// Player 2's own battle command menu: a second FieldCommandCtrl (RE-p2-command-menu.md §5.1, §10;
// RE-p2-command-menu-input.md §4.1, §10 Stage 3a; passes S-CMD1, S-CMD2).
//
// The game owns one command controller (EndUISystem[23]) and builds it with three plain calls:
// FMemory_Malloc(0x798), FieldCommandCtrl__Ctor, FieldCommandCtrl__SetupMenuWidgets(ctrl, world). The
// ctor registers the controller with the game's GC-referenced list and Setup creates its own widgets,
// states and request arrays. Player 2's controller is built the same way, once. Like the game's, it is
// never freed. It follows the game's lifecycle at the game's own calls (RE-p2-command-menu-levers.md §2):
// the one call of ResetAll and the one call of Setup on the game's controller, which every path reaches
// (field entry and exit, the fade-deferred Setup, the Blueprint thunks, the menu-close re-Setup), are
// redirected; each is made again on player 2's controller while player 2 plays. When player 2 joins, or
// was out at the game's Setup, a join Setup runs once the game's controller is set up.
// Setup makes focus calls (a list selection; a layer push when battle input is live), so every call on
// player 2's controller runs inside player 2's focus scope (player_two/focus_scope.h) and never touches
// player 1's focus.
//
// Its menu widget stays at render opacity 0 until it is shown in player 2's pass only. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_menu_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick while player 2 plays: builds player 2's controller, runs the join Setup when it does
/// not follow the game's current setup, and logs its bytes on change.
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

/// Player 2's key guide (a second KeyGuide53), or null if it could not be built.
void* KeyGuide() noexcept;

/// Player 2's controller, or null before it is built.
void* Controller() noexcept;

}  // namespace lss::player_two::command_menu_two
