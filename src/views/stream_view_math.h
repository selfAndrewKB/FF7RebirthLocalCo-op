#pragma once

// A camera as the game's streaming takes it (views/stream_views.h): location, unit forward vector
// and horizontal field of view. Pure, so it is tested on the host.

#include <cstddef>
#include <optional>

namespace lss::views {

struct StreamCamera
{
    float location[3]  = {};
    float direction[3] = {};
    float fov          = 0.0f;  // horizontal degrees, as the view was drawn
};

/// The unit forward vector of a rotation's pitch and yaw in degrees (stock FRotator::Vector: yaw
/// about Z from +X, pitch up from the XY plane).
void ForwardOf(float pitch, float yaw, float (&out)[3]) noexcept;

/// The horizontal FOV (degrees) a 16:9 frustum needs to cover a view `hfov` degrees wide whose drawn
/// area is `aspect` wide per unit of height: the game's slave-view frustum assumes 16:9, so a narrower
/// view (a side-by-side half) needs a wider FOV for its full height. Unchanged for 16:9 or wider, and
/// for unusable inputs.
float CoveringFov16x9(float hfov, float aspect) noexcept;

/// The camera of a filled FMinimalViewInfo (views/minimal_view_info.h): Location +0x00, Rotation
/// (pitch, yaw, roll) +0x0C, FOV +0x18. Empty if a value is not finite or the FOV is not in (0, 180).
std::optional<StreamCamera> CameraOf(const std::byte* viewInfo) noexcept;

}  // namespace lss::views
