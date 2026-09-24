#pragma once

// Turning one player's view: the arithmetic, kept apart from the engine so it is tested on the host.
// Only the yaw of the FMinimalViewInfo (views/minimal_view_info.h) is changed.

#include "views/minimal_view_info.h"

#include <cstddef>

namespace lss::views {

/// `degrees` brought into (-180, 180].
float NormalizeDegrees(float degrees) noexcept;

/// Adds `degrees` to the view's yaw if `self` is `turnedPlayer` (and neither is null).
/// Returns true if it turned the view.
bool TurnIfPlayer(const void* self, const void* turnedPlayer, float degrees, std::byte* viewInfo) noexcept;

}  // namespace lss::views
