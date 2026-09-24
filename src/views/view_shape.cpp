#include "views/view_shape.h"

#include "views/minimal_view_info.h"

#include <cmath>
#include <cstring>

namespace lss::views {

namespace {

constexpr float         kMinAspectRatio = 0.25f;
constexpr float         kMaxAspectRatio = 8.0f;
constexpr float         kMinFov         = 1.0f;
constexpr float         kMaxFov         = 179.0f;

float ReadFloat(const std::byte* viewInfo, std::size_t offset) noexcept
{
    float value = 0.0f;
    std::memcpy(&value, viewInfo + offset, sizeof(value));
    return value;
}

}  // namespace

bool LooksLikeViewInfo(const std::byte* viewInfo) noexcept
{
    if (viewInfo == nullptr)
    {
        return false;
    }
    // The flags byte cannot be checked: only its two low bits are defined (minimal_view_info.h).
    const float aspectRatio = ReadFloat(viewInfo, view_info::kAspectRatio);
    const float fov         = ReadFloat(viewInfo, view_info::kFov);
    return std::isfinite(aspectRatio) && aspectRatio >= kMinAspectRatio && aspectRatio <= kMaxAspectRatio &&
           std::isfinite(fov) && fov > kMinFov && fov < kMaxFov;
}

bool FillHalf(std::byte* viewInfo) noexcept
{
    if (viewInfo == nullptr)
    {
        return false;
    }
    const auto flags = static_cast<unsigned char>(viewInfo[view_info::kFlags]);
    viewInfo[view_info::kFlags] = static_cast<std::byte>(flags & ~view_info::kConstrainAspectRatioBit);
    return (flags & view_info::kConstrainAspectRatioBit) != 0;
}

}  // namespace lss::views
