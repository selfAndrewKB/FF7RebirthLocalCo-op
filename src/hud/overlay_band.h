#pragma once

// Where Square's HUD must be laid out so that a split view shows all of it, and the host scale its
// world markers must use there. Pure arithmetic, tested on the host (tests/overlay_band_test.cpp).
//
// The HUD is one Slate overlay (SEndRootOverlay) rendered into one 16:9 UI image per frame, which
// a post-process pass blends into every view. That pass fits the image to the view as a fixed 16:9
// frame, centred, by cover: scale = max(viewW/imageW, viewH/imageH), so a view of another shape
// shows only a band of the image (RE-p2-hud.md; the fit measured in game, 2026-09-25). Laying the
// overlay out inside that band, instead of the whole image, shows the whole HUD, anchored as a
// native screen of the view's shape would anchor it.
//
// Markers (EndUI__ProjectWorldToRootOverlay) convert a view point to UI units with
// k = UIScale(layout rect) / hostScale, where UIScale is EndUI__ContainScaleFor1920x1080 of the
// layout rect. A marker lands on its object when k * fitScale = 1, so the host scale must be
// UIScale(band) * fitScale. With the whole image in a solo view this is the engine's own value.

#include <cstdint>

namespace lss::hud {

/// FIntRect as EndUI__GetCompositeLayerRect fills it: Min then Max, in UI-image pixels.
struct IntRect
{
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t maxX = 0;
    std::int32_t maxY = 0;

    std::int32_t Width() const noexcept { return maxX - minX; }
    std::int32_t Height() const noexcept { return maxY - minY; }
    bool         operator==(const IntRect&) const = default;
};

/// The part of an `imageWidth` x `imageHeight` UI image that a `viewWidth` x `viewHeight` view
/// shows under the centred cover fit. The whole image when the view has the image's shape, and an
/// empty rect for unusable sizes.
IntRect VisibleBand(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept;

/// EndUI__ContainScaleFor1920x1080 (0x1409F52B0): the scale of a UI rect relative to 1920x1080,
/// reproduced instruction for instruction (RE-p2-hud.md pass 4): 1 for a size within 1e-8 of zero;
/// width/1920 when width/height is below 16/9 or unordered; otherwise height/1080; at least 0.01.
float ContainScaleFor1920x1080(float width, float height) noexcept;

/// The overlay's layout scale with the mod's layout-scale change (hud/layout_scale.h): the contain
/// rule min(height/1080, width/1920), computed as the redirected instructions compute it. For the
/// 16:9 images the game makes this is exactly its own height/1080.
float OverlayLayoutScale(std::int32_t width, std::int32_t height) noexcept;

/// The clip-space map that moves a draw covering the whole target onto `band` of it, unstretched:
/// x' = scaleX * x + translateX * w, y' = scaleY * y + translateY * w (w the clip w).
struct ClipMap
{
    float scaleX     = 1.0f;
    float scaleY     = 1.0f;
    float translateX = 0.0f;
    float translateY = 0.0f;
};
ClipMap BandClipMap(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

/// Applies `map` to a row-major FMatrix (clip = v * M; column 3 is clip w), in place.
void ApplyClipMap(float (&matrix)[16], const ClipMap& map) noexcept;

/// The host scale that puts markers on their objects when the overlay is laid out in `band` of the
/// image and the image is cover-fitted to the view: UIScale(band) * coverScale (the marker factor
/// is UIScale / host; UIScale is the game's contain scale, which the layout scale now equals);
/// 0 for unusable sizes.
float MarkerHostScale(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth,
                      float viewHeight) noexcept;

/// The x/y terms of the overlay entry's projection matrix (FEndMenuLayerEntry+0x50..+0x8F) that
/// draw a layout of `band`'s size unstretched into `band` of an `imageWidth` x `imageHeight`
/// target. The game builds this matrix from the rect's size alone, which stretches any rect over
/// the whole target (FEndMenuLayerEntry__OrthoMatrixFromRectSize 0x142F83508); for the whole image
/// these are exactly its terms. The z terms are the game's and are left alone.
struct EntryPlacement
{
    float scaleX     = 0.0f;  // +0x50  row 0 x
    float scaleY     = 0.0f;  // +0x64  row 1 y
    float translateX = 0.0f;  // +0x80  row 3 x
    float translateY = 0.0f;  // +0x84  row 3 y
};
EntryPlacement PlaceInBand(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

}  // namespace lss::hud
