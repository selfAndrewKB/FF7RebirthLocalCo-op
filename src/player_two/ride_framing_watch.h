#pragma once

// Logs each player's camera slot 15, the ride framing, and who changes it (read-only; logging for the chocobo
// camera bug, RE-p2-chocobo-warp-camera.md §4 and §6).
//
// While a player rides, slot 15 of their camera's Field operator holds the chocobo camera module the boarding
// installed. On 2026-10-07 both slots changed while both players rode: player 1's emptied, player 2's held another
// module (the camera then followed the rider's head). Every slot-15 install resolves the PRIMARY camera manager, so
// the installer ran once outside and once inside player 2's primary scope; which code it was is unknown. This
// watch names it:
//   - SetModuleSlot15 (the one function both slot-15 installs call; entry redirect, passes through): which camera,
//     the old and new module, whether player 2's primary scope was held, and the call stack as research addresses;
//   - the chocobo module's Deactivate (vtable +0x18, passes through): whose module, and the call stack;
//   - each engine tick, both cameras: slot 15 (module, active, running, owner, whom a chocobo module follows), the
//     ride subject and whether the pose is finite. A line per camera when one of them changes (Info).
// Nothing is written to the game.

#include "platform/image.h"

#include <string>

namespace lss::player_two::ride_framing_watch {

/// Installs both pass-through hooks (checked bytes and vtable entry; logs what it installed).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Each engine tick (game thread).
void Tick() noexcept;

/// The camera's slot 15, the ride subject and the pose in words, for other logs (`cameraManager` may be null).
std::wstring Describe(const void* cameraManager);

}  // namespace lss::player_two::ride_framing_watch
