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

HalfShape HalfOf(std::int32_t width, std::int32_t height, bool sideBySide) noexcept
{
    if (width <= 0 || height <= 0)
    {
        return {};
    }
    const float w      = static_cast<float>(width);
    const float h      = static_cast<float>(height);
    const float aspect = sideBySide ? (w / 2.0f) / h : w / (h / 2.0f);
    if (aspect < kMinAspectRatio || aspect > kMaxAspectRatio)
    {
        return {};
    }
    return {aspect, sideBySide ? 1.0f : 0.5f};
}

float SplitFov(float fovDegrees, float cameraAspect, const HalfShape& half, float weight) noexcept
{
    constexpr float kDegreesToHalfRadians = 3.14159265f / 360.0f;
    const bool usable = std::isfinite(fovDegrees) && fovDegrees > kMinFov && fovDegrees < kMaxFov &&
                        std::isfinite(cameraAspect) && cameraAspect >= kMinAspectRatio && cameraAspect <= kMaxAspectRatio &&
                        std::isfinite(half.aspect) && half.aspect >= kMinAspectRatio && half.aspect <= kMaxAspectRatio &&
                        std::isfinite(half.heightRatio) && half.heightRatio > 0.0f && half.heightRatio <= 1.0f &&
                        std::isfinite(weight) && weight >= 0.0f && weight <= 1.0f;
    if (!usable)
    {
        return fovDegrees;
    }
    const float tanHalfSoloVertical = std::tan(fovDegrees * kDegreesToHalfRadians) / cameraAspect;
    const float tanHalfVertical     = tanHalfSoloVertical * std::pow(half.heightRatio, weight);
    const float result = std::atan(half.aspect * tanHalfVertical) / kDegreesToHalfRadians;  // full horizontal angle
    return result > kMinFov && result < kMaxFov ? result : fovDegrees;
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
