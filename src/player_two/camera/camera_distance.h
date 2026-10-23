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
//
// Split framing (roadmap 2b, RE-p2-framing.md §6.1): while player 2 is in, each player's field operator is
// driven to the Options level plus a step offset for the layout, through the same per-camera-manager setter,
// so a half's wider or taller view can sit further back. One step adds 10 % to the row's distance and height
// (EndParam CameraLengthFieldScale; the FOV step is 0). The setter is called only when an initialised
// operator's level differs from that target; when player 2 leaves, player 1's goes back to the Options level.
// Game thread; inert unless player 2 is in.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::camera_distance {

/// Each redirect is optional and logged; false if neither could be installed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The field steps added in each layout (clamped to -2..5); 0 in both at start (the Options level).
void SetLayoutOffsets(std::int32_t topBottom, std::int32_t sideBySide) noexcept;
/// One layout's steps as set: 0 top/bottom, 1 side by side (0 for any other).
std::int32_t LayoutOffset(int layout) noexcept;
bool         FramingAvailable() noexcept;

/// Each engine tick, with whether player 2 is in.
void TickFraming(bool playerTwoIn) noexcept;

#if LSS_DEV
/// The `framing-offset.top-bottom` and `framing-offset.side-by-side` commands and readings. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::player_two::camera_distance
