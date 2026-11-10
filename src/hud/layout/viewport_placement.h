#pragma once

// Where a HUD root goes in a split half (pure; no game access). The rule behind hud/layout/hud_placement.h
// and hud/field/action_icon_base.h: every HUD root is designed for a 1920x1080 frame, and in a split half
// keeps its single-player distance from the corner or edge it belongs to.

#include "hud/layout/overlay_band.h"

#include <cstdint>
#include <optional>

namespace lss::hud {

/// UUserWidget's viewport layout: what SetAnchorsInViewport, SetPositionInViewport (offsets Left,
/// Top), SetDesiredSizeInViewport (offsets Right, Bottom) and SetAlignmentInViewport set.
struct ViewportLayout
{
    float anchors[4]   = {};  // Min X, Min Y, Max X, Max Y
    float offsets[4]   = {};  // Left, Top, Right, Bottom
    float alignment[2] = {};

    bool operator==(const ViewportLayout&) const = default;
};

inline constexpr float kHudFrameWidth  = 1920.0f;
inline constexpr float kHudFrameHeight = 1080.0f;

/// The nearest of a frame axis's start (0), middle (0.5) and end (1), by thirds.
float NearestAnchor(float position, float frame) noexcept;

/// A viewport layout a widget could really have: finite, anchors within 0..1, alignment within
/// -1..2, offsets within +-100000.
bool Plausible(const ViewportLayout& layout) noexcept;

/// The layout for a split half, from the game's own `own`: a 1920x1080 area at (anchorX, anchorY)
/// of the half, aligned by its matching corner or edge. Its contents keep their design positions
/// inside it. In a 1920x1080 layout this lands exactly where the game's own layout does. Nullopt
/// for an implausible `own`.
std::optional<ViewportLayout> SplitLayoutFor(float anchorX, float anchorY, const ViewportLayout& own) noexcept;

/// A point in layout units (an FVector2D).
struct LayoutPoint
{
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const LayoutPoint&) const = default;
};

/// The corner or edge of the 1920x1080 frame a point belongs to, by thirds on each axis (the
/// field-action icons' slot-0 base (70, 957) belongs to the bottom left: (0, 1)).
LayoutPoint AnchorOf(const LayoutPoint& point) noexcept;

/// `original`, a position in the 1920x1080 frame, moved so it keeps its distance from `anchor` in a
/// layout of `width` x `height` units: original + anchor * (layout - 1920x1080). Unchanged in a
/// 1920x1080 layout, and for a zero layout (unknown).
LayoutPoint KeepDistanceFromAnchor(const LayoutPoint& original, const LayoutPoint& anchor, float width,
                                   float height) noexcept;

/// The point the game must be given, in layout units, for a UI effect to land on `point` of the layout. The UI
/// effects (the summon gauge's glow) are placed from a layout point through the overlay's layout size, centred and in
/// half-heights, and drawn against the whole `imageWidth` x `imageHeight` UI image, while the layout itself is drawn
/// into `band` of that image. So `point` is mapped as the band's drawing maps it, and that image position is given
/// back in the layout's own units. `point` itself in a layout filling the image (a 1920x1080 view), and for an unknown
/// layout, band or image.
LayoutPoint EffectPointFor(const LayoutPoint& point, const LayoutSize& layout, const IntRect& band,
                           std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

/// Whether a widget placed at `position` (its top-left, in layout units) starts outside a layout of
/// `width` x `height` units, where it is not painted (and so not ticked). False for an unknown
/// (zero) layout.
bool OutsideLayout(const LayoutPoint& position, float width, float height) noexcept;

}  // namespace lss::hud
