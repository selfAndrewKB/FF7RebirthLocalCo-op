#include "hud/overlay_band.h"

#include <algorithm>
#include <cmath>

namespace lss::hud {

namespace {

bool Usable(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    return imageWidth > 0 && imageHeight > 0 && std::isfinite(viewWidth) && std::isfinite(viewHeight) &&
           viewWidth >= 1.0f && viewHeight >= 1.0f;
}

float CoverScale(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    return std::max(viewWidth / static_cast<float>(imageWidth), viewHeight / static_cast<float>(imageHeight));
}

}  // namespace

IntRect VisibleBand(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    if (!Usable(imageWidth, imageHeight, viewWidth, viewHeight))
    {
        return {};
    }
    // The view covers imageSize * scale; the band is the view's own size back in image pixels.
    const float scale = CoverScale(imageWidth, imageHeight, viewWidth, viewHeight);
    const auto  bandW = std::min(imageWidth, static_cast<std::int32_t>(std::lround(viewWidth / scale)));
    const auto  bandH = std::min(imageHeight, static_cast<std::int32_t>(std::lround(viewHeight / scale)));
    const auto  x     = (imageWidth - bandW) / 2;
    const auto  y     = (imageHeight - bandH) / 2;
    return {x, y, x + bandW, y + bandH};
}

float ContainScaleFor1920x1080(float width, float height) noexcept
{
    constexpr float kEpsilon = 1e-8f;
    if (std::fabs(width) <= kEpsilon || std::fabs(height) <= kEpsilon)
    {
        return 1.0f;
    }
    const float scale = width / height < 16.0f / 9.0f ? width / 1920.0f : height / 1080.0f;
    return std::max(scale, 0.01f);
}

float MarkerHostScale(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth,
                      float viewHeight) noexcept
{
    if (!Usable(imageWidth, imageHeight, viewWidth, viewHeight) || band.Width() <= 0 || band.Height() <= 0)
    {
        return 0.0f;
    }
    return ContainScaleFor1920x1080(static_cast<float>(band.Width()), static_cast<float>(band.Height())) *
           CoverScale(imageWidth, imageHeight, viewWidth, viewHeight);
}

EntryPlacement PlaceInBand(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    if (imageWidth <= 0 || imageHeight <= 0 || band.Width() <= 0 || band.Height() <= 0)
    {
        return {};
    }
    // A layout pixel p lands on target pixel band.Min + p:
    //   NDC.x = 2 (Min.x + p.x) / W - 1,   NDC.y = 1 - 2 (Min.y + p.y) / H.
    const float w = static_cast<float>(imageWidth);
    const float h = static_cast<float>(imageHeight);
    return {2.0f / w, -2.0f / h, 2.0f * static_cast<float>(band.minX) / w - 1.0f,
            1.0f - 2.0f * static_cast<float>(band.minY) / h};
}

}  // namespace lss::hud
