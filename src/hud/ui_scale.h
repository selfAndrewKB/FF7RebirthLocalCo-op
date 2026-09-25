#pragma once

// The overlay's UI scale in bands narrower than 16:9 (side-by-side halves).
//
// EndUI__ContainScaleFor1920x1080 turns a UI size into "UI pixels per layout unit": width/1920
// below 16:9, height/1080 otherwise. The overlay always lays out at height/1080, so the two agree
// only for 16:9 and wider, which is every image the game makes itself (RE-p2-hud.md pass 4). Its
// callers (EndUI__GetUIScale's 128 call sites, and three that pass the overlay size) use it to
// convert projected positions into layout units, so in a tall band markers land doubled away
// from the band's corner.
//
// The function is replaced at its entry (platform/code_redirect.h, its first instruction) by one
// that reproduces it exactly (hud/overlay_band.h, ContainScaleFor1920x1080) and differs only when
// its input is the band the overlay is laid out in (hud/overlay_layout.h) and that band is
// narrower than 16:9: then it returns the layout scale. Every other size, from every caller, gets
// the game's value. The whole function's bytes are checked first, so the copy is known to match.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

/// Checks the function's bytes and replaces it; false (nothing changed) if they differ.
bool InstallOverlayUIScale(const platform::GameImage& image) noexcept;

/// Puts the game's function back (at mod uninstall, with player 2 out).
void RemoveOverlayUIScale() noexcept;

}  // namespace lss::hud
