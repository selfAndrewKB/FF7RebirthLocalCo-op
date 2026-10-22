#pragma once

// Player 2's command menu widgets per half (RE-p2-command-menu-views.md §10.3, §10.5). A controller's command menu,
// its field menu (the party list) and its wait-mode filter (the tactical-mode tint), and the key guide's widget (the
// game's at EndUISystem[53], player 2's own), are each an FWeakObjectPtr; player 2's stay at render opacity 0 until
// player 2's pass of the overlay render shows them instead of player 1's. A unit of player 2's command menu
// (player_two/command_menu_two.h). Game thread (the render passes run on it).

namespace lss::player_two::command_menu_view {

/// Each engine tick, with or without player 2: player 2's menu widget kept at render opacity 0 (in
/// battle something outside the controller shows every command widget; RE-p2-command-menu-input.md §12).
void KeepHidden() noexcept;

/// Before a pass of the overlay render (0 player 1, 1 player 2): in battle, player 2's pass shows player
/// 2's command menu (at player 1's menu's render opacity) instead of player 1's; player 1's pass shows
/// player 1's only. Out of battle player 2's stays hidden. Render thread = game thread.
void ShowOnly(int player) noexcept;

/// After the frame's passes: player 1's menu back to its own opacity, player 2's hidden.
void Restore() noexcept;

}  // namespace lss::player_two::command_menu_view
