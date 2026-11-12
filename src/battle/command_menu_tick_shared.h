#pragma once

// What the command menu tick's units share (battle/command_menu_tick.h): the pass in flight and the tick's
// reads the route and the tint need, the controller's request indices and their reader. Internal; not a public API.

#include "battle/command_menu_tick.h"

#include "facts/command_controller.h"
#include "players/object_fields.h"

#include <cstdint>

namespace lss::battle::command_menu_tick {

/// The controller's request array's indices (research/RE-p2-command-menu-tick.md).
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

/// Sets the pass in flight and its seat (the route: 3 around a seat's press or release in its UI, 0 after; the seat
/// is kept for passes 2 and 3 only).
void SetPass(int pass, int seat) noexcept;

/// The stage is installed: its calls are placed and no seat's tick has faulted.
bool Installed() noexcept;

/// The live address of the EndUISystem object, and of the global holding the party manager (0 before Install).
std::uintptr_t UiSystem() noexcept;
std::uintptr_t PartyManager() noexcept;

}  // namespace lss::battle::command_menu_tick
