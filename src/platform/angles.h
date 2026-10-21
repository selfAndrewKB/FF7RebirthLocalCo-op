#pragma once

// Angle units, and the one quaternion reading the mod makes.
//
// The game keeps rotators and field-of-view angles in degrees and rotations as FQuat {x, y, z, w}; the mod's
// trigonometry wants radians. Pure; usable from the host tests.

#include <cmath>
#include <numbers>

namespace lss::platform {

inline constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
inline constexpr float kDegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

/// The yaw (degrees, -180..180) of a unit quaternion's X axis: the direction a transform faces, which the game
/// compares a stick's direction with (the approach's end transform, a ledge's edge rotation).
inline float QuaternionYawDegrees(float x, float y, float z, float w) noexcept
{
    return std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (y * y + z * z)) * kDegreesPerRadian;
}

}  // namespace lss::platform