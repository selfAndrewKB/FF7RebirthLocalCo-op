#pragma once

// Logs what player 2's camera does over a follow warp of player 2's member (read-only).
//
// The Field operator re-pins its pivot to its subject every frame, so a camera left at the old spot after
// the warp has three static causes (RE-p2-party-warp.md §11.1): (A) the operator's subject no longer names
// or resolves to the member, (B) no operator is current, so UpdateCamera skips the camera actor, or (C) a
// non-finite pose. From the warp's start to a few seconds after, each tick samples all three, the pivot,
// the member and the camera actor; a line is written when one of them changes and when the pivot catches
// up, then a verdict: followed, or LEFT BEHIND with the whole sample (then once a second for a while, to
// see whether it recovers). Nothing is written to the game.
// Game thread.

namespace lss::player_two::warp_camera_watch {

/// A follow warp of `member` was just requested: start watching player 2's camera.
void Arm(void* member) noexcept;

/// Each engine tick, with player 2's current member (null when none).
void Tick(void* member, float seconds) noexcept;

}  // namespace lss::player_two::warp_camera_watch
