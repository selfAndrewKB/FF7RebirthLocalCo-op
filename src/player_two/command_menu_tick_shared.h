#pragma once

// What the command menu tick's units share (player_two/command_menu_tick.h): the pass in flight and the tick's
// reads the route and the tint need, the controller's request indices and their reader. Internal; not a public API.

#include "player_two/command_menu_tick.h"

#include "facts/command_controller.h"
#include "player_two/object_fields.h"

#include <cstdint>

namespace lss::player_two::command_menu_tick {

/// The controller's request array's indices (RE-p2-command-menu-tick.md §2).
enum Request : std::int32_t
{
    kPrevMember = 0,
    kNextMember = 1,
    kReqAttack  = 12,
    kReqUnique  = 13,
    kReqGuardDodge = 32,
    kReqGuard   = 37,
};

using facts::kCmdRequests;

/// The controller's request at `index` (its request array: data, then num), or -1 past the array.
inline std::int32_t RequestOf(std::uintptr_t controller, std::int32_t index) noexcept
{
    const auto data = ReadAt<std::uintptr_t>(controller + kCmdRequests);
    return data != 0 && index < ReadAt<std::int32_t>(controller + kCmdRequests + 8)
               ? ReadAt<std::int32_t>(data + 4 * static_cast<std::uint32_t>(index))
               : -1;
}

/// Sets the pass in flight (the route: 3 around pad 2's press or release in player 2's UI, 0 after).
void SetPass(int pass) noexcept;

/// The stage is installed: its calls are placed and player 2's tick has not faulted.
bool Installed() noexcept;

/// The live address of the EndUISystem object, and of the global holding the party manager (0 before Install).
std::uintptr_t UiSystem() noexcept;
std::uintptr_t PartyManager() noexcept;

}  // namespace lss::player_two::command_menu_tick
