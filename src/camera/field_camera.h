#pragma once

// Each seat's camera (players/seats.h): the field camera's settings, and the seat's own camera update.
//
// Settings (the Field operator's policy). Each camera manager owns a set of camera operators; the
// Field operator (+0x2CB0) frames the player in the field. The game sets it up only on player 1's
// camera manager: activation, deactivation and the 0x58-byte command all reach "the primary camera
// manager's" operator, at an area load and on the game's camera orders. The replacements for those
// three slots of the Field operator's vtable repeat each call on every seat's Field operator, in seat
// order, activating first and copying the command last (the first activation writes the command's
// defaults). When a seat's player takes a party member, Seed brings that seat's operator to player 1's
// current state once, since no area load follows.
//
// Not under a cutscene or special camera (research/RE-p2-shop-camera-skip.md section 12):
// while another operator than the Field (or the Battle) sits on top of player 1's camera manager, the
// cut's own tick pushes its rotation into player 1's Field command every frame through the same
// slot; mirrored, those pinned the seat's camera to the cut's rotation, and the copy at the cut's end
// turned it to player 1's angle. So no command is mirrored then (counted), and a seat joining then
// gets the state only; the first command once the Field operator is on top again is mirrored whole.
//
// Update (AEndPlayerCameraManager::UpdateCamera, slot 211). Each camera manager's update asks the
// party manager for "the" player controller: its stick input, and the helpers that find "the"
// camera manager. The replacement finds the seat from the camera manager it is given and runs that
// seat's update with the primary controller scoped to the seat (scopes/primary_controller.h),
// so it reads the seat's input and its own operators. It acts only once the seat's Field operator is
// active and the camera manager's current operator index names an existing operator, since helpers
// inside the update use both without checking. In battle, a seat's update also runs in that seat's UI
// context and target fields (command_menu, lock_on). A seat riding along with the member a script took
// (players::generation::RideMemberOf) is updated as the seat's too: its controller keeps that member as its leader, so
// the camera follows it and the player's stick turns it.
//
// Per seat: the counters, the mismatch count and the count of commands not mirrored under a cut. Game thread.

#include "players/stage_counters.h"

namespace lss::platform {
class GameImage;
}

namespace lss::camera::field_camera {

/// Replaces the three Field-operator slots. False (logged) if any check fails; then none is replaced.
bool InstallSettings(const platform::GameImage& image) noexcept;

/// Replaces the camera update slot. False (logged) if the check fails.
bool InstallUpdate(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// What Seed did.
enum class Seeded
{
    NoPair,              // the operators could not be looked up (not installed, or not distinct): nothing changed
    Matches,             // the state and the command (outside the rotation) agree with player 1's
    Mismatch,            // copied, yet they do not agree
    PlayerOneUnderOther, // player 1's camera is under a cutscene or special camera: the state brought over, the
                         // command left the seat's own (the game's per-frame commands under a cut are not the field's)
};

/// Brings `seat`'s Field operator to player 1's state and, while player 1's Field (or Battle) operator is the top
/// operator of player 1's camera manager, player 1's command. Game thread, after the seat's player has taken a party
/// member.
Seeded Seed(int seat) noexcept;

/// The log's words for a Seed result.
const wchar_t* Describe(Seeded seeded) noexcept;

/// The game's commands to player 1's Field operator not mirrored since the last call, because player 1's camera was
/// under a cutscene or special camera (its per-frame rotation pushes), for the status line.
unsigned TakeNotMirrored(int seat) noexcept;

/// `seat`'s counts for the status line.
players::StageCounters& SettingsCounters(int seat) noexcept;
players::StageCounters& UpdateCounters(int seat) noexcept;

/// `seat`'s calls where the mirrored command or active state did not match player 1's afterwards.
unsigned TakeMismatches(int seat) noexcept;
}  // namespace lss::camera::field_camera
