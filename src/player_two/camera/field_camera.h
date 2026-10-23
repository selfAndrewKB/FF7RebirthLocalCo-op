#pragma once

// Player 2's camera: the field camera's settings, and player 2's own camera update.
//
// Settings (the Field operator's policy). Each camera manager owns a set of camera operators; the
// Field operator (+0x2CB0) frames the player in the field. The game sets it up only on player 1's
// camera manager: activation, deactivation and the 0x58-byte policy command all reach "the primary
// camera manager's" operator, once per area load. The replacements for those three slots of the
// Field operator's vtable repeat each call on player 2's Field operator, activating first and
// copying the command last (activation re-initialises the command bytes). When player 2 takes a
// party member, Seed brings player 2's operator to player 1's current state once, since no area
// load follows.
//
// Update (AEndPlayerCameraManager::UpdateCamera, slot 211). Each camera manager's update asks the
// party manager for "the" player controller: its stick input, and the helpers that find "the"
// camera manager. The replacement runs player 2's update with the primary controller scoped to
// player 2 (player_two/scopes/primary_controller.h), so it reads player 2's input and its own operators.
// It acts only once player 2's Field operator is active and the camera manager's current operator
// index names an existing operator, since helpers inside the update use both without checking.

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

/// Brings player 2's Field operator to player 1's state and command. Game thread, after player 2
/// has taken a party member. Returns true if both operators agree afterwards.
bool Seed() noexcept;

/// Counts for the status line.
StageCounters& SettingsCounters() noexcept;
StageCounters& UpdateCounters() noexcept;

/// Calls where the mirrored command or active state did not match player 1's afterwards.
unsigned TakeMismatches() noexcept;

}  // namespace lss::player_two::field_camera
