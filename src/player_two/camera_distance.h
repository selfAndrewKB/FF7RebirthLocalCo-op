#pragma once

// Player 2's camera follows the Options camera distance (RE-p2-battle-camera.md §6, pass S-CAMDIST1).
//
// Each camera manager's field and battle operators take the distance level from the options once, at
// their own first use; a later change reaches a camera manager only when the Options menu applies it,
// and the game applies it to the primary one only. The Options menu applies it from two places: its
// option setter calls PrimaryPCM_ApplyCameraDistanceOptions(field, battle), and its confirm-key handler
// calls AEndPlayerCameraManager__ApplyCameraDistanceOptions(pcm, field, battle) on the primary camera
// manager. Both calls are redirected: the game's call runs, then the game's per-camera-manager setter
// runs for player 2's camera manager with the same values. The setter touches only the camera manager
// it is given and starts the operators' own blend. The values come from the call's arguments: at both
// sites the options data is written back only after the call.
// Game thread; inert unless player 2 is in.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::camera_distance {

/// Each redirect is optional and logged; false if neither could be installed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::player_two::camera_distance
