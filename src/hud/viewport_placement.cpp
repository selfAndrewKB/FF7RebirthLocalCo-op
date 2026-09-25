#include "hud/viewport_placement.h"

#include <cmath>
#include <cstddef>

namespace lss::hud {

namespace {

bool AllWithin(const float* values, std::size_t count, float low, float high) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(values[i]) || values[i] < low || values[i] > high)
        {
            return false;
        }
    }
    return true;
}

bool AnchoredTopLeft(const ViewportLayout& layout) noexcept
{
    return layout.anchors[0] == 0.0f && layout.anchors[1] == 0.0f && layout.anchors[2] == 0.0f &&
           layout.anchors[3] == 0.0f;
}

void SetPointAnchor(ViewportLayout& layout, float x, float y) noexcept
{
    layout.anchors[0] = layout.anchors[2] = x;
    layout.anchors[1] = layout.anchors[3] = y;
}

}  // namespace

float NearestAnchor(float position, float frame) noexcept
{
    return position < frame / 3.0f ? 0.0f : position > frame * 2.0f / 3.0f ? 1.0f : 0.5f;
}

bool Plausible(const ViewportLayout& layout) noexcept
{
    return AllWithin(layout.anchors, 4, 0.0f, 1.0f) && AllWithin(layout.alignment, 2, -1.0f, 2.0f) &&
           AllWithin(layout.offsets, 4, -100000.0f, 100000.0f);
}

std::optional<ViewportLayout> SplitLayoutFor(PlacementKind kind, float anchorX, float anchorY,
                                             const ViewportLayout& own) noexcept
{
    if (!Plausible(own))
    {
        return std::nullopt;
    }
    ViewportLayout layout = own;
    if (kind == PlacementKind::Area)
    {
        SetPointAnchor(layout, anchorX, anchorY);
        layout.alignment[0] = anchorX;
        layout.alignment[1] = anchorY;
        layout.offsets[0]   = 0.0f;
        layout.offsets[1]   = 0.0f;
        layout.offsets[2]   = kHudFrameWidth;
        layout.offsets[3]   = kHudFrameHeight;
        return layout;
    }
    if (!AnchoredTopLeft(own))
    {
        return std::nullopt;
    }
    const float ax = NearestAnchor(own.offsets[0], kHudFrameWidth);
    const float ay = NearestAnchor(own.offsets[1], kHudFrameHeight);
    SetPointAnchor(layout, ax, ay);
    layout.offsets[0] = own.offsets[0] - ax * kHudFrameWidth;
    layout.offsets[1] = own.offsets[1] - ay * kHudFrameHeight;
    return layout;
}

}  // namespace lss::hud
