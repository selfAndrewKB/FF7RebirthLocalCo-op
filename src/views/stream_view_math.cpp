#include "views/stream_view_math.h"

#include "views/minimal_view_info.h"

#include <cmath>
#include <cstring>

namespace lss::views {

namespace {

using view_info::kFov;
using view_info::kLocation;
using view_info::kPitch;
using view_info::kYaw;

constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;

float FloatAt(const std::byte* base, std::size_t offset) noexcept
{
    float value = 0.0f;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

}  // namespace

void ForwardOf(float pitch, float yaw, float (&out)[3]) noexcept
{
    const float p = pitch * kRadiansPerDegree;
    const float y = yaw * kRadiansPerDegree;
    out[0] = std::cos(p) * std::cos(y);
    out[1] = std::cos(p) * std::sin(y);
    out[2] = std::sin(p);
}

float CoveringFov16x9(float hfov, float aspect) noexcept
{
    if (!(hfov > 0.0f && hfov < 180.0f) || !(aspect > 0.0f) || !std::isfinite(aspect))
    {
        return hfov;
    }
    constexpr float kWide = 16.0f / 9.0f;
    const float half    = std::tan(hfov * kRadiansPerDegree / 2.0f);
    const float needed  = 2.0f * std::atan(half * kWide / aspect) / kRadiansPerDegree;
    return needed > hfov ? needed : hfov;
}

std::optional<StreamCamera> CameraOf(const std::byte* viewInfo) noexcept
{
    if (viewInfo == nullptr)
    {
        return std::nullopt;
    }
    StreamCamera camera;
    for (std::size_t i = 0; i < 3; ++i)
    {
        camera.location[i] = FloatAt(viewInfo, kLocation + i * sizeof(float));
        if (!std::isfinite(camera.location[i]))
        {
            return std::nullopt;
        }
    }
    const float pitch = FloatAt(viewInfo, kPitch);
    const float yaw   = FloatAt(viewInfo, kYaw);
    camera.fov        = FloatAt(viewInfo, kFov);
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || !(camera.fov > 0.0f && camera.fov < 180.0f))
    {
        return std::nullopt;
    }
    ForwardOf(pitch, yaw, camera.direction);
    return camera;
}

}  // namespace lss::views
