#pragma once

// What player 2's command menu's units share (player_two/command_menu_two.h): whether a game call may be made again
// on player 2's controller, whether the stage is live, and the UI system's address. Internal; not a public API.

#include "player_two/command_menu_two.h"

#include <cstdint>

namespace lss::player_two::command_menu_two {

/// The game's call may be made again on player 2's controller: it is built, we are on the game thread, the call
/// was made on the game's controller (the UI system's slot names it), and, when the call needs player 2's member
/// (Setup stores the leader's type), player 2 plays.
bool CanFollow(void* gameController, bool needsPlayer) noexcept;

/// The stage is installed and no call of the game's has faulted on player 2's controller.
bool Ready() noexcept;

/// The live address of the EndUISystem object (0 before Install, or after one whose check of it failed).
std::uintptr_t UiSystem() noexcept;

}  // namespace lss::player_two::command_menu_two
