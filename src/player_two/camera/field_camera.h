#pragma once

// Each seat's camera (players/seats.h): the field camera's settings, and the seat's own camera update.
//
// Settings (the Field operator's policy). Each camera manager owns a set of camera operators; the
// Field operator (+0x2CB0) frames the player in the field. The game sets it up only on player 1's
// camera manager: activation, deactivation and the 0x58-byte policy command all reach "the primary
// camera manager's" operator, once per area load. The replacements for those three slots of the
// Field operator's vtable repeat each call on every seat's Field operator, in seat order, activating
// first and copying the command last (activation re-initialises the command bytes). When a seat's
// player takes a party member, Seed brings that seat's operator to player 1's current state once,
// since no area load follows.
//
// Update (AEndPlayerCameraManager::UpdateCamera, slot 211). Each camera manager's update asks the
// party manager for "the" player controller: its stick input, and the helpers that find "the"
// camera manager. The replacement finds the seat from the camera manager it is given and runs that
// seat's update with the primary controller scoped to the seat (player_two/scopes/primary_controller.h),
// so it reads the seat's input and its own operators. It acts only once the seat's Field operator is
// active and the camera manager's current operator index names an existing operator, since helpers
// inside the update use both without checking. In battle, player 2's update also runs in player 2's UI
// context and target fields (command_menu_two, lock_on: player 2's until the battle group's step).
//
// Per seat: the counters and the mismatch count. Game thread.

#include "player_two/stage_counters.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::field_camera {

/// Replaces the three Field-operator slots. False (logged) if any check fails; then none is replaced.
bool InstallSettings(const platform::GameImage& image) noexcept;

/// Replaces the camera update slot. False (logged) if the check fails.
bool InstallUpdate(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Brings `seat`'s Field operator to player 1's state and command. Game thread, after the seat's player
/// has taken a party member. Returns true if both operators agree afterwards.
bool Seed(int seat) noexcept;

/// `seat`'s counts for the status line.
StageCounters& SettingsCounters(int seat) noexcept;
StageCounters& UpdateCounters(int seat) noexcept;

/// `seat`'s calls where the mirrored command or active state did not match player 1's afterwards.
unsigned TakeMismatches(int seat) noexcept;

}  // namespace lss::player_two::field_camera
