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

/// The render-transform pivot (a fraction of the widget's own size) that scales a widget about the `anchor` corner of its
/// `width` x `height` area with its render translation scaled too. Slate draws a point p of the widget at
/// pivot + scale * (p - pivot) + translation, the translation after the scale; with the pivot at corner - translation
/// that is corner + scale * (p + translation - corner): the widget as the game places it, scaled about the corner. A
/// widget without a translation gets the corner itself. Nullopt for an empty area.
std::optional<LayoutPoint> CornerPivot(const LayoutPoint& anchor, const LayoutPoint& translation, float width,
                                       float height) noexcept;

/// The corner or edge of the 1920x1080 frame a point belongs to, by thirds on each axis (the
/// field-action icons' slot-0 base (70, 957) belongs to the bottom left: (0, 1)).
LayoutPoint AnchorOf(const LayoutPoint& point) noexcept;

/// `original`, a position in the 1920x1080 frame, moved so it keeps its distance from `anchor` in a
/// layout of `width` x `height` units: original + anchor * (layout - 1920x1080). Unchanged in a
/// 1920x1080 layout, and for a zero layout (unknown).
LayoutPoint KeepDistanceFromAnchor(const LayoutPoint& original, const LayoutPoint& anchor, float width,
                                   float height) noexcept;

/// The UI particle pass's viewport in one column's image, as fractions of the image: a box's centre and height (its
/// width follows from the target's shape at the draw).
struct EffectBox
{
    float centreX = 0.0f;
    float centreY = 0.0f;
    float height  = 0.0f;

    bool operator==(const EffectBox&) const = default;
};

/// Where the UI particle pass's viewport goes in one column's image so that column's UI particles land where its HUD
/// is (hud/passes/ui_effect_pass.h). The game converts each effect's layout point once a frame, centred in `tickLayout`
/// and in its half-heights (the 1920x1080 frame while the mod answers the conversion's size, else the layout the
/// overlay holds at the widget tick: player 1's column's), and every column's pass draws that one result over its
/// whole image; the sprites' sizes are in half-heights of the box. The box makes `tickLayout`'s half-height the band's
/// drawing of this column's `layout` units (its height = the band's times tickLayout's height over the layout's) and
/// puts the centre where `tickLayout`'s centre falls in this column's band, moved by the distance the widget the points
/// belong to (its area anchored at `anchor` of the 1920x1080 frame) stands from its design place in `layout`; with that
/// widget drawn at `size` about `scaleCentre` (a fraction of its 1920x1080 area; its anchored corner when empty;
/// hud/layout/hud_placement.h, the HUD size), the box is `size` times as tall and its centre moves with the widget's
/// points. The image's centre and height for a 16:9 column while `tickLayout` is 16:9 too at size 1; past an edge of the
/// image for a wide or tall column, for a 16:9 one beside a wide `tickLayout`, or above size 1 at a bottom or right
/// corner. Nullopt for an unknown layout, band, image, size or centre.
std::optional<EffectBox> EffectViewport(const LayoutSize& tickLayout, const LayoutSize& layout, const IntRect& band,
                                        std::int32_t imageWidth, std::int32_t imageHeight, const LayoutPoint& anchor,
                                        float size = 1.0f, std::optional<LayoutPoint> scaleCentre = std::nullopt) noexcept;

/// Whether a widget placed at `position` (its top-left, in layout units) starts outside a layout of
/// `width` x `height` units, where it is not painted (and so not ticked). False for an unknown
/// (zero) layout.
bool OutsideLayout(const LayoutPoint& position, float width, float height) noexcept;

}  // namespace lss::hud
