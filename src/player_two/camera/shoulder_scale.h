#pragma once

// The camera's sideways (shoulder) offset keeps its single-player look in a split half (the user, 2026-10-01).
//
// The game's camera places itself from a pose whose AimOffset Y is a sideways offset in world units: rows such as
// InDoorNormal (40 cm) and the SpaceOffset module (away from walls) set it. That offset is an angle off the view's
// centre, so the same offset fills about twice the screen share in a side-by-side half (32 deg wide) as in the
// single-player view (60 deg), and less in a wide top/bottom half. Nothing in the game's camera reads the view's
// shape (RE-p2-framing.md §5), and no per-player lever for the offset exists, so this is a documented exception:
// just before each camera operator's FinalizePose (the call at 0x140A61C20 in its module update), the pose's
// AimOffset Y is multiplied by that player's width ratio, tan(half's horizontal FOV / 2) / tan(camera's / 2)
// (views::SplitWidthRatio), kept at or above a minimum (0.7 at start, the user's pick) and capped at 1: a half wider
// than the camera (top/bottom) keeps the game's own offset, so the mod only ever softens it (the user, 2026-10-01).
// The game then places the camera, with its own location and
// collision steps, from the scaled pose; the next update rebuilds the pose from rows and modules, so nothing
// accumulates.
// Game thread; only while the split is shown.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::shoulder_scale {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;
bool Installed() noexcept;

/// On at start; `shoulder-scale off` leaves the game's offset unscaled.
void SetOn(bool on) noexcept;
bool On() noexcept;

/// The smallest factor (0.25..1; 0.7 at start): a narrow half's offset is softened no further than this.
void SetMinimum(float minimum) noexcept;
float Minimum() noexcept;

/// Each engine tick: the halves' width ratios against the camera's (views::SplitWidthRatio, player 1's and player
/// 2's), kept for the finalizer; a line when a player's factor changes.
void Poll(bool splitShown, float ratioOne, float ratioTwo) noexcept;

#if LSS_DEV
/// The `shoulder-scale` and `shoulder-scale-min` commands and readings. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::player_two::shoulder_scale
