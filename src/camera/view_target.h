#pragma once

// Pointing player 2's camera manager at its own camera actor.
//
// The End camera manager's BeginPlay spawns a camera actor and makes it the view target, and the
// Field operator positions that actor behind the followed character. For a camera manager spawned
// while the world is already playing, BeginPlay runs inside SpawnActor, and the controller then
// calls InitializeFor, which sets the view target to the controller itself. The view then comes
// from the controller: the pawn's position, the default field of view, and no camera arm.
//
// Aim repeats BeginPlay's own call, with BeginPlay's own arguments, after the controller is set up.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::camera::view_target {

/// Finds SetViewTarget and the camera-manager vtable. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// True if the controller's End camera manager views its own camera actor.
bool OnOwnCamera(void* controller) noexcept;

/// What the controller's camera manager views now (null if it has no End camera manager).
void* Current(void* controller) noexcept;

/// Makes the controller's End camera manager view its own camera actor. Returns OnOwnCamera
/// afterwards; false without a call if the camera manager is not an End one that the controller
/// owns, or it has no camera actor.
bool Aim(void* controller) noexcept;

/// Makes the controller's End camera manager view `actor` (another player's camera actor, say), with the same
/// call and arguments as Aim. True if it views the actor afterwards; false without a call under Aim's conditions.
/// The mirror views use it (views/mirror_player.h): a mirror's camera manager shows player 1's camera.
bool AimAt(void* controller, void* actor) noexcept;

}  // namespace lss::camera::view_target
