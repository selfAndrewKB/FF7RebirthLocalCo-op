#pragma once

// The minimap's map rendered once per frame, by player 1's HUD pass (RE-oom-path.md, "The 32 KB leak").
//
// In settlements the HUD shows a minimap (the open world shows a compass). Its circle widget
// (SEndNaviMapCircle) renders the map into its own render target through the menu renderer's pool 0,
// which runs a 3-slot ring; a slot's batch arrays go back to their free lists only in a later RHI-thread
// command, while the game thread may reuse the slot before that command runs, and every array of that
// frame is then stranded for good. Each further player's HUD image (hud/passes/second_image.h) paints the overlay
// once more each frame, so the ring turns that much faster and the 32 KB pool fills in minutes (confirmed live:
// hud-image off stops the climb, on brings it back).
//
// Every pass paints the same widget into the same render target, the further players' first, and the overlay is
// drawn only after player 1's pass, so a further player's map render is overwritten before anything shows it.
// In a further player's pass this skips that render (see the .cpp for the exact sites); that player's minimap shows
// the map player 1's pass rendered, as it already did. The minimap stays a copy of player 1's.
//
// Logged (Debug, once a minute while a minimap is up): each widget's map renders and skips per period. The reading
// of pool 0's batch arrays that watched the leak was retired once the fix was confirmed (2026-10-21).
// The switch (`minimap-once on|off`) is on from install.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallMinimapOnce(const platform::GameImage& image) noexcept;
void RemoveMinimapOnce() noexcept;
bool MinimapOnceInstalled() noexcept;

void SetMinimapOnce(bool on) noexcept;
bool MinimapOnceOn() noexcept;

/// Game thread, around a seat's overlay render (hud/passes/root_overlay_render.cpp): the skip byte is set for it
/// only, and not while the seat's pass paints the seat's minimap (hud/minimap/minimap.h).
void BeginMinimapSeatPass(bool seatMinimap) noexcept;
void EndMinimapSeatPass() noexcept;

/// Game thread, each engine tick: the per-period counts line.
void PollMinimapOnce() noexcept;

#if LSS_DEV
/// The `minimap-once` command and reading. May throw (out of memory).
void RegisterMinimapOnceCommands();
#endif

}  // namespace lss::hud
