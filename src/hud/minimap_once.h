#pragma once

// The minimap's map rendered once per frame, by player 1's HUD pass (RE-oom-path.md, "The 32 KB leak").
//
// In settlements the HUD shows a minimap (the open world shows a compass). Its circle widget
// (SEndNaviMapCircle) renders the map into its own render target through the menu renderer's pool 0,
// which runs a 3-slot ring; a slot's batch arrays go back to their free lists only in a later RHI-thread
// command, while the game thread may reuse the slot before that command runs, and every array of that
// frame is then stranded for good. Player 2's HUD image (hud/second_image.h) paints the overlay a second
// time each frame, so the ring turns twice as fast and the 32 KB pool fills in minutes (confirmed live:
// hud-image off stops the climb, on brings it back).
//
// Both passes paint the same widget into the same render target, player 2's first, and the overlay is
// drawn only after player 1's pass, so player 2's map render is overwritten before anything shows it.
// In player 2's pass this skips that render (see the .cpp for the exact sites); player 2's minimap shows
// the map player 1's pass rendered, as it already did. The minimap stays a copy of player 1's.
//
// Logged: pool-0 submits per pass and skips, and, once a second while the minimap is up, the circle
// entry's batch arrays (in use and free), which climb when arrays are stranded.
// The switch (`minimap-once on|off`) is on from install.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallMinimapOnce(const platform::GameImage& image) noexcept;
void RemoveMinimapOnce() noexcept;
bool MinimapOnceInstalled() noexcept;

void SetMinimapOnce(bool on) noexcept;
bool MinimapOnceOn() noexcept;

/// Game thread, around player 2's overlay render (hud/second_image.cpp): the skip byte is set for it only, and not
/// while player 2's pass paints player 2's minimap (hud/minimap.h).
void BeginMinimapPlayerTwoPass(bool playerTwoMinimap) noexcept;
void EndMinimapPlayerTwoPass() noexcept;

/// Game thread, each engine tick: the per-period counts line and the batch-array reading.
void PollMinimapOnce() noexcept;

}  // namespace lss::hud
