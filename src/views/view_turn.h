#pragma once

// Turning one player's view: the arithmetic, kept apart from the engine so it is tested on the host.
//
// FMinimalViewInfo, as ULocalPlayer::GetViewPoint fills it in this build
// (RE-local-player-viewpoint.md §Q3): Location at +0x00 (3 floats), Rotation Pitch/Yaw/Roll at
// +0x0C/+0x10/+0x14 (degrees), FOV at +0x18. Only the yaw is changed.

#include <cstddef>

namespace lss::views {

namespace view_info {
inline constexpr std::size_t kYaw = 0x10;  // FRotator::Yaw, degrees
/// PreviousViewTransform's "is set" byte (a TOptional's flag). Read only, for the log.
inline constexpr std::size_t kPreviousViewTransformSet = 0x690;
}  // namespace view_info

/// `degrees` brought into (-180, 180].
float NormalizeDegrees(float degrees) noexcept;

/// Adds `degrees` to the view's yaw if `self` is `turnedPlayer` (and neither is null).
/// Returns true if it turned the view.
bool TurnIfPlayer(const void* self, const void* turnedPlayer, float degrees, std::byte* viewInfo) noexcept;

}  // namespace lss::views
