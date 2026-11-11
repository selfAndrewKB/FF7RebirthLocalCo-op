#include "hud/layout/viewport_placement.h"

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

std::optional<ViewportLayout> SplitLayoutFor(float anchorX, float anchorY, const ViewportLayout& own) noexcept
{
    if (!Plausible(own))
    {
        return std::nullopt;
    }
    ViewportLayout layout = own;
    SetPointAnchor(layout, anchorX, anchorY);
    layout.alignment[0] = anchorX;
    layout.alignment[1] = anchorY;
    layout.offsets[0]   = 0.0f;
    layout.offsets[1]   = 0.0f;
    layout.offsets[2]   = kHudFrameWidth;
    layout.offsets[3]   = kHudFrameHeight;
    return layout;
}

LayoutPoint AnchorOf(const LayoutPoint& point) noexcept
{
    return {NearestAnchor(point.x, kHudFrameWidth), NearestAnchor(point.y, kHudFrameHeight)};
}

LayoutPoint KeepDistanceFromAnchor(const LayoutPoint& original, const LayoutPoint& anchor, float width,
                                   float height) noexcept
{
    if (!(width > 0.0f) || !(height > 0.0f) || !std::isfinite(width) || !std::isfinite(height))
    {
        return original;
    }
    return {original.x + anchor.x * (width - kHudFrameWidth), original.y + anchor.y * (height - kHudFrameHeight)};
}

std::optional<EffectBox> EffectViewport(const LayoutSize& tickLayout, const LayoutSize& layout, const IntRect& band,
                                        std::int32_t imageWidth, std::int32_t imageHeight, const LayoutPoint& anchor) noexcept
{
    if (!(tickLayout.width > 0.0f) || !(tickLayout.height > 0.0f) || !(layout.width > 0.0f) || !(layout.height > 0.0f) ||
        band.Width() <= 0 || band.Height() <= 0 || imageWidth <= 0 || imageHeight <= 0 || !std::isfinite(anchor.x) ||
        !std::isfinite(anchor.y))
    {
        return std::nullopt;
    }
    // The band's drawing: layout units onto the band's pixels. The game's result is in half-heights of `tickLayout`,
    // and the draw puts one half-height at half the box, so the box is tickLayout's height in the band's pixels per
    // unit. The game converts the design point against `tickLayout`'s centre, so a point at that centre lands at the
    // box's centre; in this column the widget the points belong to stands anchor * (layout - frame) from its design
    // place, so the centre goes there too.
    const float scale   = static_cast<float>(band.Height()) / layout.height;
    const float centreX = static_cast<float>(band.minX) +
                          (tickLayout.width / 2.0f + anchor.x * (layout.width - kHudFrameWidth)) * scale;
    const float centreY = static_cast<float>(band.minY) +
                          (tickLayout.height / 2.0f + anchor.y * (layout.height - kHudFrameHeight)) * scale;
    const float height = tickLayout.height * scale;
    return EffectBox{centreX / static_cast<float>(imageWidth), centreY / static_cast<float>(imageHeight),
                     height / static_cast<float>(imageHeight)};
}

bool OutsideLayout(const LayoutPoint& position, float width, float height) noexcept
{
    if (!(width > 0.0f) || !(height > 0.0f))
    {
        return false;
    }
    return !(position.x >= 0.0f && position.x < width && position.y >= 0.0f && position.y < height);
}

}  // namespace lss::hud
