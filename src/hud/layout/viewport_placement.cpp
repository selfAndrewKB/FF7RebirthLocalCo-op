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

LayoutPoint EffectPointFor(const LayoutPoint& point, const LayoutSize& layout, const IntRect& band,
                           std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    if (!(layout.width > 0.0f) || !(layout.height > 0.0f) || band.Width() <= 0 || band.Height() <= 0 || imageWidth <= 0 ||
        imageHeight <= 0 || !std::isfinite(point.x) || !std::isfinite(point.y))
    {
        return point;
    }
    // The band's drawing: layout units onto the band's pixels of the image.
    const float imageX = static_cast<float>(band.minX) + point.x * (static_cast<float>(band.Width()) / layout.width);
    const float imageY = static_cast<float>(band.minY) + point.y * (static_cast<float>(band.Height()) / layout.height);
    // The effects' own placement, inverted: from the layout's centre, in its half-heights, against the image's centre
    // and half-height.
    const float unit = layout.height / static_cast<float>(imageHeight);
    return {layout.width / 2.0f + (imageX - static_cast<float>(imageWidth) / 2.0f) * unit,
            layout.height / 2.0f + (imageY - static_cast<float>(imageHeight) / 2.0f) * unit};
}

float EffectSizeFactor(const LayoutSize& layout, const IntRect& band, std::int32_t imageHeight) noexcept
{
    if (!(layout.height > 0.0f) || band.Height() <= 0 || imageHeight <= 0)
    {
        return 1.0f;
    }
    const float bandScale = static_cast<float>(band.Height()) / layout.height;
    const float soloScale = static_cast<float>(imageHeight) / kHudFrameHeight;
    return bandScale / soloScale;
}

int ReferenceColumn(const LayoutSize* layouts, int count) noexcept
{
    if (layouts == nullptr || count <= 0)
    {
        return 0;
    }
    const auto known = [](const LayoutSize& l) { return l.width > 0.0f && l.height > 0.0f; };
    // The same shape within 1 % on each side: a band a pixel wider (rounding, a DPI step) is still that shape.
    const auto same = [](const LayoutSize& a, const LayoutSize& b) {
        return std::fabs(a.width - b.width) <= 0.01f * b.width && std::fabs(a.height - b.height) <= 0.01f * b.height;
    };
    int best      = 0;
    int bestCount = 0;
    for (int i = 0; i < count; ++i)
    {
        if (!known(layouts[i]))
        {
            continue;
        }
        int n = 0;
        for (int j = 0; j < count; ++j)
        {
            n += known(layouts[j]) && same(layouts[i], layouts[j]) ? 1 : 0;
        }
        if (n > bestCount)  // a tie keeps the earlier column: player 1's wins a tie with the shapes after it
        {
            best      = i;
            bestCount = n;
        }
    }
    return best;
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
