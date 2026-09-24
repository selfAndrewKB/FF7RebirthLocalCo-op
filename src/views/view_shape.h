#pragma once

// How a split player's view fills its part of the screen: the arithmetic on the FMinimalViewInfo
// (views/minimal_view_info.h), kept apart from the engine so it is tested on the host.
//
// The game's cameras ask for 16:9 with bConstrainAspectRatio set, so the engine keeps each split
// half 16:9 and fills the rest with bars. Clearing the flag lets the view take its whole half; the
// field of view then follows ULocalPlayer::AspectRatioAxisConstraint, which the mod sets to
// MajorAxisFOV (keep the FOV of the half's longer side). RE-vertical-split-and-fill.md.

#include <cstddef>
#include <cstdint>

namespace lss::views {

/// EAspectRatioAxisConstraint::AspectRatio_MajorAxisFOV: a side-by-side half (taller than wide)
/// keeps the vertical FOV, a top/bottom half (wider than tall) keeps the horizontal FOV.
inline constexpr std::uint8_t kMajorAxisFov = 2;

/// True if the struct holds an aspect ratio and a FOV a real camera could have, as this build's
/// FMinimalViewInfo does at those offsets. A struct that fails is not written.
bool LooksLikeViewInfo(const std::byte* viewInfo) noexcept;

/// Clears bConstrainAspectRatio so the view fills its part of the screen. The aspect ratio itself
/// is kept: the FOV rule reads it. Returns true if the flag was set.
bool FillHalf(std::byte* viewInfo) noexcept;

}  // namespace lss::views
