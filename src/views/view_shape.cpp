#include "views/view_shape.h"

#include "platform/angles.h"
#include "views/minimal_view_info.h"

#include <cmath>
#include <cstring>

namespace lss::views {

namespace {

constexpr float kMinAspectRatio = 0.25f;
constexpr float kMaxAspectRatio = 8.0f;
constexpr float kMinFov         = 1.0f;
constexpr float kMaxFov         = 179.0f;

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

float WidthRatio(float splitFovDegrees, float cameraFovDegrees) noexcept
{
    constexpr float kHalfAngle = platform::kRadiansPerDegree / 2.0f;
    const float     ratio      = std::tan(splitFovDegrees * kHalfAngle) / std::tan(cameraFovDegrees * kHalfAngle);
    return std::isfinite(ratio) && ratio > 0.0f ? ratio : 1.0f;
}

float VerticalFovDegrees(float horizontalFovDegrees, float aspect) noexcept
{
    constexpr float kHalfRadians = platform::kRadiansPerDegree / 2.0f;
    return std::atan(std::tan(horizontalFovDegrees * kHalfRadians) / aspect) / kHalfRadians;
}

}  // namespace lss::views
