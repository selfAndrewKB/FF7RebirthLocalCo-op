#pragma once

// Player 1's UI focus, logged on change (RE-p2-command-menu-input.md §2, §10 Stage 3a, pass S-CMD2). Read-only; a dev
// tool (dev/dev_tools.h installs it; a player's build has the empty stubs below).
//
// UI actions reach one focus path only: the focused widget and its ancestors, kept by Square's input
// object (EndUISystem[2]) with a priority-sorted stack of focus layers. The command menu is the bottom
// layer (priority -1000); an open list focuses its item. Player 2's command menu (player_two/command_menu_two.h)
// never touches this context; the log shows the path (each widget's Slate type, and a UMG host's user-widget
// class), the layers (widget, saved focus, priority) and the command controller's focus bytes when they change.
// Game thread only.

#include "dev/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::menu_focus {

#if LSS_DEV

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per frame while player 2 plays: player 1's focus logged on change.
void Watch() noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void Remove() noexcept {}
inline void Watch() noexcept {}

#endif

}  // namespace lss::player_two::menu_focus
