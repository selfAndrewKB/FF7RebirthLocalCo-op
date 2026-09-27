#pragma once

// Player 1's UI focus, logged for player 2's command menu (RE-p2-command-menu-input.md §2, §10 Stage 3a,
// pass S-CMD2). Read-only.
//
// UI actions reach one focus path only: the focused widget and its ancestors, kept by Square's input
// object (EndUISystem[2]) with a priority-sorted stack of focus layers. The command menu is the bottom
// layer (priority -1000); an open list focuses its item. A second command menu for player 2 must not
// touch this context, so the log shows it before one is built: on change, the path (each widget's
// Slate type, and a UMG host's user-widget class), the layers (widget, saved focus, priority) and the
// command controller's focus bytes. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::menu_focus {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per frame while player 2 plays: player 1's focus logged on change.
void Watch() noexcept;

}  // namespace lss::player_two::menu_focus
