#pragma once

// How a split player's view fills its part of the screen: the arithmetic on the FMinimalViewInfo
// (views/minimal_view_info.h), kept apart from the engine so it is tested on the host.
//
// The game's cameras ask for 16:9 with bConstrainAspectRatio set, so the engine keeps each split
// half 16:9 and fills the rest with bars. With the flag cleared on each main camera (through the
// game's setter, player_two/main_camera_shape.h) the view takes its whole half; the field of view
// then follows ULocalPlayer::AspectRatioAxisConstraint, which the mod sets (MajorAxisFOV, or
// MaintainXFOV with the split FOV below). RE-vertical-split-and-fill.md.

#include <cstddef>
#include <cstdint>

namespace lss::views {

/// EAspectRatioAxisConstraint::AspectRatio_MajorAxisFOV: a side-by-side half (taller than wide)
/// keeps the vertical FOV, a top/bottom half (wider than tall) keeps the horizontal FOV.
inline constexpr std::uint8_t kMajorAxisFov = 2;

/// EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV: the FOV field is the horizontal FOV of the
/// view as drawn. The rule the split FOV blend writes its result for.
inline constexpr std::uint8_t kMaintainXFov = 1;

/// One player's half of a `width` x `height` viewport split in two: its width/height, and its
/// height as a fraction of the viewport's (1 side by side, 0.5 top/bottom). Zeroes if unusable.
struct HalfShape
{
    float aspect      = 0.0f;
    float heightRatio = 0.0f;
};
HalfShape HalfOf(std::int32_t width, std::int32_t height, bool sideBySide) noexcept;

/// The weight the split FOV gives to keeping the characters' on-screen size over keeping the
/// single-player framing (SplitFov); 0.5 is the geometric middle, chosen by eye on 2026-09-25.
inline constexpr float kDefaultSplitFovWeight = 0.5f;

/// The horizontal FOV (degrees) for a split half, from the camera's single-player view
/// (`fovDegrees` wide at `cameraAspect`, the 16:9 the game's cameras ask for). A split view can
/// keep the single-player *framing* (the same vertical FOV, so a character sits at the same
/// fraction of the height) or the single-player *scale* (the same pixels per degree, so a
/// character is as many pixels tall, which needs a vertical FOV in proportion to the half's
/// height). Side by side, a half is as tall as the screen and the two agree. Top/bottom they
/// conflict, and `weight` picks between them:
///     tan(vertical/2) = tan(solo vertical/2) * heightRatio^weight
/// (0: framing kept; 1: scale kept). Returns `fovDegrees` unchanged for unusable inputs.
float SplitFov(float fovDegrees, float cameraAspect, const HalfShape& half, float weight) noexcept;

/// True if the struct holds an aspect ratio and a FOV a real camera could have, as this build's
/// FMinimalViewInfo does at those offsets. A struct that fails is not written.
bool LooksLikeViewInfo(const std::byte* viewInfo) noexcept;

}  // namespace lss::views
