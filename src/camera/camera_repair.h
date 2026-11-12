#pragma once

// A seat's camera with a non-finite pose gets the game's own player-warp camera reset
// (research/RE-p2-chocobo-warp-camera.md, RE-p2-party-warp.md).
//
// After a follow warp the Field operator's pose pitch can become NaN (on foot and riding). The stick
// rotation adds to last frame's pitch and its clamp keeps a NaN, so the camera never recovers: it stays at the old
// spot and the seat's player is lost. When the game warps the player on purpose it calls ResetCameraToField(cut), which
// re-arms the Reset module; that module writes a fresh pitch and yaw straight into the pose. The follow warp never
// calls it, because a follower is no camera's subject.
//
// Each engine tick, for each seat (players/seats.h) whose player plays a member and whose Field operator is current:
// a pose whose camera values are not finite (the three vectors' fourth lanes are stack residue and do not count;
// world::world_rules::kPoseLanes) runs the game's PrimaryPCM_ResetCameraToField(1) inside the seat's primary scope (it
// resolves the primary camera manager), so it lands on the seat's camera and player 1's is not touched. At most one
// try per 0.5 s and 5 in a row until the pose is finite again. Each repair is logged with the camera's state before,
// after one frame and after ten (Info).
// Per seat: the tries, the follow-up and the timers; the seats are repaired in turn, in seat order. Game thread.

#include "platform/image.h"

namespace lss::camera::camera_repair {

/// Finds the game's reset (checked bytes; logs the outcome).
void Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the session's: every built seat in turn.
void Tick() noexcept;

}  // namespace lss::camera::camera_repair
