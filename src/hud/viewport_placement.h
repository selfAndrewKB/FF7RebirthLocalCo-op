#pragma once

// Where a HUD root's viewport layout goes in a split half (pure; no game access). The rule behind
// hud/hud_placement.h: every HUD root is designed for a 1920x1080 frame, and in a split half keeps
// its single-player distance from the corner or edge it belongs to.

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

/// How a HUD root lays itself out in its 1920x1080 frame.
enum class PlacementKind
{
    Area,   // fills the layout; its contents sit at their design positions inside it
    Point,  // the game places it at a point of the frame (anchors (0,0), offsets = the point)
};

inline constexpr float kHudFrameWidth  = 1920.0f;
inline constexpr float kHudFrameHeight = 1080.0f;

/// The nearest of a frame axis's start (0), middle (0.5) and end (1), by thirds.
float NearestAnchor(float position, float frame) noexcept;

/// A viewport layout a widget could really have: finite, anchors within 0..1, alignment within
/// -1..2, offsets within +-100000.
bool Plausible(const ViewportLayout& layout) noexcept;

/// The layout for a split half, from the game's own `own`:
///   Area: a 1920x1080 area at (anchorX, anchorY) of the half, aligned by its matching corner or edge;
///   Point: the same point, anchored to the frame's nearest corner or edge and offset from it by the
///          same distance (nullopt if `own` is not anchored at (0,0)).
/// In a 1920x1080 layout both land exactly where the game's own layout does. Nullopt for an
/// implausible `own`.
std::optional<ViewportLayout> SplitLayoutFor(PlacementKind kind, float anchorX, float anchorY,
                                             const ViewportLayout& own) noexcept;

}  // namespace lss::hud
