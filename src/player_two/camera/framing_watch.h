#pragma once

// Logs each player's field-camera framing while the split is shown (read-only; roadmap 2b).
//
// Each camera manager's Field operator holds 8 camera-setting layers (rows of the game's CameraSetting
// table), blends the active ones into one state (FOV, Distance, Height, AimOffset), scales Distance,
// Height and FOV by the Camera Distance level (`1 + (level - 1) * k`, k from three EndParam values) and
// builds the pose from it (RE-p2-framing.md §1-§2); modules then adjust the pose. A line per player is written
// when the active rows, the level, the active modules or the pose's aim offset (the shoulder, 5 cm steps) change,
// at most 4 a second, and every 30 s; the three k values once. Nothing is written to the game.
// Game thread.

#include "platform/image.h"

namespace lss::player_two::framing_watch {

/// Resolves the three EndParam getters (checked entry bytes). Without them the k values are not logged.
void Install(const platform::GameImage& image) noexcept;

/// Each engine tick.
void Tick(bool splitShown) noexcept;

}  // namespace lss::player_two::framing_watch
