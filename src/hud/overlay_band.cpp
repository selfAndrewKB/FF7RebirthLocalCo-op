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

namespace {

// The constants the game's function reads, as stored (0x1460A2684, 0x1460A2A20, 0x14656D540,
// 0x1460A3B30, 0x1460A2660, 0x1460A26F4).
constexpr float kContainEpsilon = 1e-8f;
constexpr float kSixteenNinths  = 1.7777778f;
constexpr float kInv1920        = 1.0f / 1920.0f;
constexpr float kInv1080        = 1.0f / 1080.0f;
constexpr float kMinScale       = 0.01f;

/// vmaxss x, x, 0.01: the second operand when the first is not greater (NaN included).
float AtLeastMin(float scale) noexcept
{
    return scale > kMinScale ? scale : kMinScale;
}

}  // namespace

float ContainScaleFor1920x1080(float width, float height) noexcept
{
    // vcomiss + jbe: "not above" includes unordered.
    if (!(std::fabs(width) > kContainEpsilon) || !(std::fabs(height) > kContainEpsilon))
    {
        return 1.0f;
    }
    // vcomiss + jb: "below" includes unordered, so a NaN ratio takes the width branch.
    const float ratio = width / height;
    return AtLeastMin(!(ratio >= kSixteenNinths) ? width * kInv1920 : height * kInv1080);
}

float OverlayLayoutScale(std::int32_t width, std::int32_t height) noexcept
{
    const float byHeight = static_cast<float>(height) * kInv1080;
    const float byWidth  = static_cast<float>(width) * kInv1920;
    return byHeight < byWidth ? byHeight : byWidth;  // vminss xmm0, xmm0, xmm1: the second when not less
}

ClipMap BandClipMap(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    if (imageWidth <= 0 || imageHeight <= 0 || band.Width() <= 0 || band.Height() <= 0)
    {
        return {};
    }
    const float w = static_cast<float>(imageWidth);
    const float h = static_cast<float>(imageHeight);
    const float bw = static_cast<float>(band.Width());
    const float bh = static_cast<float>(band.Height());
    return {bw / w, bh / h, (2.0f * static_cast<float>(band.minX) + bw) / w - 1.0f,
            1.0f - (2.0f * static_cast<float>(band.minY) + bh) / h};
}

void ApplyClipMap(float (&matrix)[16], const ClipMap& map) noexcept
{
    for (int row = 0; row < 4; ++row)
    {
        float* m = matrix + row * 4;
        m[0]     = map.scaleX * m[0] + map.translateX * m[3];
        m[1]     = map.scaleY * m[1] + map.translateY * m[3];
    }
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
