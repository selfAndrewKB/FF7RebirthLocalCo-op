#pragma once

// Split halves fill their part of the screen by the game's own camera setting.
//
// Each camera manager (AEndPlayerCameraManager) spawns one plain ACameraActor at BeginPlay, keeps it
// at +0x2DE8 and makes it its view target; every frame UpdateCamera writes the camera operators'
// result into it. Its CameraComponent keeps the ACameraActor constructor's bConstrainAspectRatio =
// true at 16:9, and no game code clears it (RE-p2-hud-view-levers.md §3). That flag is what keeps a
// split half 16:9 with bars.
//
// While the halves fill (a further player in, filled halves chosen), each player's main camera gets the
// stock Blueprint setter UCameraComponent::SetConstraintAspectRatio(false) once, the same store the
// game would make. The flag's original value is remembered and set back through the same setter
// when the halves stop filling. A new main camera (a new camera manager, e.g. a further player's) is found
// and set on the tick it appears. AspectRatio stays 16:9: the FOV rule reads it.
//
// Cameras other than the main camera (level-sequence and dialogue cutscene cameras) keep their own
// flag, so a cutscene letterboxes to 16:9 inside the half, as the game does on any non-16:9 screen.
// The view point's shape log (views/player_view_point.cpp) shows any view still constrained.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::camera::main_camera_shape {

/// Checks the camera manager's vtable; the reflected setter and fields are found at first use.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, every tick: `fill` = the split halves fill their part of the screen. Clears each
/// player's main camera's constraint once while true; sets every changed camera back when false.
void Update(bool fill) noexcept;

}  // namespace lss::camera::main_camera_shape
