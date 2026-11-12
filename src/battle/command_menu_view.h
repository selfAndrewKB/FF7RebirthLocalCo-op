#pragma once

// Each seat's command menu widgets per view (research/RE-p2-command-menu-views.md).
// A controller's command menu, its field menu (the party list) and its wait-mode filter (the tactical-mode
// tint), and the key guide's widget (the game's at EndUISystem[53], each seat's own), are each an FWeakObjectPtr; the
// seats' stay at render opacity 0 until the seat's pass of the overlay render shows its own instead of player 1's. A
// unit of the seats' command menus (battle/command_menu.h). Game thread (the render passes run on it).

namespace lss::battle::command_menu_view {

/// Each engine tick, with or without a seat in: each seat's menu widgets kept at render opacity 0 (in
/// battle something outside the controller shows every command widget; research/RE-p2-command-menu-input.md).
void KeepHidden() noexcept;

/// Before a pass of the overlay render (`seat` 0 player 1's): in battle, a seat's pass shows the seat's command menu
/// (at player 1's menu's render opacity) instead of player 1's and every other seat's; player 1's pass shows player
/// 1's only. Out of battle the seats' stay hidden. Render thread = game thread.
void ShowOnly(int seat) noexcept;

/// After the frame's passes: player 1's menu back to its own opacity, every seat's hidden.
void Restore() noexcept;

}  // namespace lss::battle::command_menu_view
