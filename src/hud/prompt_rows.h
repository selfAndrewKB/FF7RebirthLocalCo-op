#pragma once

// The field prompt rows (FieldAction_Command_C): each player sees their own, in their own half
// (RE-p2-field-prompts.md).
//
// Every prompt row the game shows is a widget in one global list (EndUISystem+0x208: a pointer to
// {FWeakObjectPtr* data, int32 num}). The rows have no link back to their owner, but their owners
// keep weak pointers to them:
//   - each player controller's leader-action component (PC+0x598) has 14 row handles: a request
//     byte at +0x9A8+12*i and the row's weak widget at +0x9AC+12*i (hookshot/"Terrain Action",
//     parkour, catch, memo...). Player 2's component detects from player 2's own character, so its
//     rows are player 2's;
//   - everything else (the talk/examine manager, which follows the leader; the move-state summons;
//     minigame and encounter registrars) is player 1's.
// A row is player 2's while player 2's component names it, and stays so until it leaves the list
// (its removal animation runs after the component has let go of it).
//
// Once per frame before the overlay renders (while player 2 is in):
//   - Refresh reads and classifies the list. When it changed, each player's rows are laid out on
//     their own with the game's own layout (FieldActionIconList__SortAndLayout on a list of that
//     player's rows), so each half stacks only its own rows from the top slot; the game lays out the
//     whole list only when a row is added or removed, and this runs after it in the same frame.
//     Changes are logged.
//   - Around each overlay pass, ShowOnly hides the other player's rows (UWidget::SetVisibility,
//     Hidden) and Restore puts back each row's own visibility, so the game's show/hide animations
//     are untouched between frames.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::prompt_rows {

bool Install(const platform::GameImage& image) noexcept;

/// Before the overlay renders, while player 2 is in.
void Refresh() noexcept;

/// Before an overlay pass: only `player`'s rows (0 = player 1, 1 = player 2) stay shown.
void ShowOnly(int player) noexcept;

/// After the passes: every row hidden here gets its own visibility back.
void Restore() noexcept;

}  // namespace lss::hud::prompt_rows
