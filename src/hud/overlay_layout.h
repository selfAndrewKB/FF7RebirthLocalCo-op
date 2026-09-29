#pragma once

// Lays Square's HUD out inside the part of its UI image a split view shows, so each half shows the
// whole HUD with its world markers on their objects (hud/overlay_band.h has the arithmetic).
//
// Three calls of the overlay's own per-frame render (SEndRootOverlay__RenderToCompositeLayer, game
// thread) are redirected (platform/call_redirect.h); all are inert unless player 2 is in and the
// band is switched on:
//   - EndUI__GetCompositeLayerRect returns the visible band instead of the whole image, so the
//     widget tree lays itself out at the band's size and anchors (a native 32:9 or 8:9 screen);
//   - FindOrAddNamedLayerEntry's entry gets the projection terms that draw that layout into the
//     band unstretched (the game's matrix ignores the rect's position), and the overlay's host
//     scale (+0x330) becomes the one that keeps markers on their objects. Both are written after
//     the game's own writes and before any widget runs, every frame the band applies.
//   - SubmitLayerSlot: just before the entry goes to the renderer, the per-child view-projections
//     widgets painted in 3D pushed into it (the command menu, the Members panel) get the same move
//     onto the band, as one clip-space map, the equivalent of a viewport the draw lacks.
//
// With the band switched off the redirects still run and log what they would set, so the values
// can be checked in the log before anything changes on screen.

#include "hud/overlay_band.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

/// Game thread: the layout size of the overlay's last render, in UI units: the band's while the
/// band applies, 1920x1080 otherwise.
LayoutSize LastLayoutSize() noexcept;

/// Checks every fact in hud/hud_addresses.h and installs the three redirects and the code patches
/// (hud/centered_frame_code.h, hud/arrow_edge_code.h), or none.
bool InstallOverlayLayout(const platform::GameImage& image) noexcept;

/// Puts the engine's calls back (at mod uninstall, with player 2 out).
void RemoveOverlayLayout() noexcept;

bool OverlayLayoutInstalled() noexcept;

/// Game thread, every tick: whether the screen is split (player 2 in).
void SetSplitActive(bool active) noexcept;

/// The `hud-band` switch: lay the HUD out in the visible band while split (on at start).
void SetBandEnabled(bool enabled) noexcept;
bool BandEnabled() noexcept;

}  // namespace lss::hud
