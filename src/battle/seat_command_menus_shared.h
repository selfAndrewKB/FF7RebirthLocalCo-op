#pragma once

// What the further players' command menus' units share (battle/seat_command_menus.h): whether a game call may
// be made again on a seat's controller, whether its stage is live, and the UI system's address. Internal; not a public
// API.

#include "battle/seat_command_menus.h"

#include <cstdint>

namespace lss::battle::seat_command_menus {

/// The game's call may be made again on `seat`'s controller: it is built, we are on the game thread, the call was made
/// on the game's controller (the UI system's slot names it, and it is no seat's), and, when the call needs the seat's
/// member (Setup stores the leader's type), the seat plays.
bool CanFollow(int seat, void* gameController, bool needsPlayer) noexcept;

/// The stage is installed and no call of the game's has faulted on `seat`'s controller.
bool Ready(int seat) noexcept;

/// The live address of the EndUISystem object (0 before Install, or after one whose check of it failed).
std::uintptr_t UiSystem() noexcept;

}  // namespace lss::battle::seat_command_menus
