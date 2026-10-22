#pragma once

// Player 2's camera with a non-finite pose gets the game's own player-warp camera reset (RE-p2-chocobo-warp-camera.md
// §5; RE-p2-party-warp.md §11.4).
//
// After a follow warp the Field operator's pose pitch can become NaN (2026-09-30 on foot, 2026-10-07 riding). The stick
// rotation adds to last frame's pitch and its clamp keeps a NaN, so the camera never recovers: it stays at the old
// spot and player 2 is lost. When the game warps the player on purpose it calls ResetCameraToField(cut), which
// re-arms the Reset module; that module writes a fresh pitch and yaw straight into the pose. The follow warp never
// calls it, because a follower is no camera's subject.
//
// Each engine tick, while player 2 plays a member and their Field operator is current: a pose whose camera values are
// not finite (the three vectors' fourth lanes are stack residue and do not count; world_rules::kPoseLanes)
// runs the game's PrimaryPCM_ResetCameraToField(1) inside player 2's primary scope (it resolves the primary camera
// manager), so it lands on player 2's camera and player 1's is not touched. At most one try per 0.5 s and 5 in a row
// until the pose is finite again. Each repair is logged with the camera's state before, after one frame and after
// ten (Info).
// Game thread.

#include "platform/image.h"

namespace lss::player_two::camera_repair {

/// Finds the game's reset (checked bytes; logs the outcome).
void Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the session's.
void Tick() noexcept;

}  // namespace lss::player_two::camera_repair
