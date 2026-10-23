#pragma once

// Each split player's camera as a streaming view of the game's own (RE-p2-distant-lod-swap.md).
//
// The MassiveEnvironment renderer keeps one set of detail requests per chunk, which every view's
// render clears and rewrites; with two different views the renders race for the copy that streaming
// reads, and the other view's scenery drops to its lowest detail and back (research/RE-p2-lod-swap-
// live.md). The game's own way to stream around more than one camera is End_AddViewSlaveLocation,
// which a cutscene, the rail camera and the minigames call every frame for their camera: the
// renderer then computes requests for every registered camera together. While the split is on
// screen the mod registers each player's last drawn camera the same way, every engine tick, before
// the world and the streaming update run.
//
// Arguments as the game's callers pass them (research doc, "Slave-location call facts"): location,
// unit forward, the horizontal FOV in radians (widened to cover a view narrower than the 16:9 the
// slave frustum assumes), main-pass scale 0 (the game's own), 0 for the flag that would switch the slave
// frustums off for good, far 0 (unbounded, as the rail camera), static-shadow far 0 (off).

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views {

enum class StreamViewsMode : std::uint8_t
{
    Off,        // stock: the renders race (for comparison)
    PlayerTwo,  // player 2's camera only
    Both,       // player 1's and player 2's (the default)
};

/// Checks the game's function; false (logged, the call is never made) if it differs, or if the view
/// point hook that records each player's drawn camera (views/player_view_point.h) is not installed.
bool InstallStreamViews(const platform::GameImage& image) noexcept;
bool StreamViewsInstalled() noexcept;

/// Development: which cameras are registered (logged at the next update).
void SetStreamViewsMode(StreamViewsMode mode) noexcept;
StreamViewsMode GetStreamViewsMode() noexcept;

/// Development: the horizontal FOV (degrees, capped) player 1's camera is registered with in `Both`, or 0
/// for the FOV its half covers. A wider camera also asks for the scenery just outside player 1's view,
/// such as the casters of the shadows it shows (logged at the next update).
void SetPlayerOneStreamFov(float degrees) noexcept;
/// The FOV set (degrees, capped), or 0 for the FOV player 1's half covers.
float PlayerOneStreamFov() noexcept;

/// Game thread, every engine tick before the world ticks: registers the cameras while the split is
/// on screen and the game state exists (the game's function needs it).
void UpdateStreamViews(bool splitShown) noexcept;

#if LSS_DEV
/// The `stream-views` and `stream-fov-p1` commands and readings. May throw (out of memory).
void RegisterStreamCommands();
#endif

}  // namespace lss::views
