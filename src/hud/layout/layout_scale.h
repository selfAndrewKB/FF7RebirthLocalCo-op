#pragma once

// The HUD overlay's layout scale by the game's own contain rule.
//
// SEndRootOverlay__RenderToCompositeLayer lays the overlay out at scale height/1080 of its rect
// (0x140C95FC6), while every conversion from UI pixels to layout units uses the contain scale of
// the same rect, EndUI__ContainScaleFor1920x1080: height/1080 for 16:9 and wider, width/1920 for
// narrower (RE-p2-hud.md passes 4-5). The game only makes 16:9 images, where the two are equal. A
// split half narrower than 16:9 (side by side) is laid out in a band narrower than 16:9, and the
// two part: markers land doubled, and widgets the game keeps in its centred 16:9 frame fall off
// screen.
//
// The one instruction that computes the layout scale is redirected (platform/code_redirect.h) to
// instructions that compute the contain rule instead, min(height/1080, width/1920), with the
// game's own constants. The code is self-contained (no call into the mod), and for the 16:9 rects
// the game makes it gives exactly the game's value, so it changes nothing outside a tall band.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

/// Checks the surrounding instructions and redirects the layout-scale instruction; false (nothing changed, logged) if
/// the code is not what the research read or the overlay layout (hud/layout/overlay_layout.h) is not installed. Game
/// version 1.005: its instruction crosses a 16-byte block, so the two instructions before it are replaced instead, from
/// the game thread outside the overlay render (platform/deferred_sites.h).
bool InstallLayoutScale(const platform::GameImage& image) noexcept;

/// Puts the game's instruction back (at mod uninstall).
void RemoveLayoutScale() noexcept;

}  // namespace lss::hud
