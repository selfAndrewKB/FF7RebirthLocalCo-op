#pragma once

// Each local player's view point: its shape while the screen is split, and player 2's optional
// test turn.
//
// The mod replaces the ULocalPlayer vtable entry for GetViewPoint (engine_addresses.h) and calls
// the engine's function first. With one player the result is exactly the engine's. While player 2
// is in:
//   - every player's view fills its half: the game's 16:9 constraint is cleared and the FOV rule
//     becomes MajorAxisFOV (views/view_shape.h); Ctrl+F4 switches this off to compare with bars.
//     Each player's own FOV rule is put back when player 2 leaves.
//   - player 2 has no camera of its own yet: it borrows player 1's controller, so the engine gives
//     it player 1's view. For testing two different views, player 2's view can be turned 30° to
//     the side, standing in for a real second camera.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views {

inline constexpr float kTestTurnDegrees = 30.0f;

/// Installs the view-point entry. Call once, after every engine fact has been verified.
/// Returns false, changing nothing, if the entry does not hold the engine's function.
bool InstallPlayerViewPoint(const platform::GameImage& image) noexcept;

/// Puts the engine's own function back (at mod uninstall).
void RemovePlayerViewPoint() noexcept;

/// Which ULocalPlayer is player 2 (null when there is none). Game thread. A change turns the
/// test turn off, puts back the FOV rule of every remaining player the mod changed, and the next
/// view point of the new player 2 is described in the log.
void SetPlayerTwo(const void* localPlayer) noexcept;

/// The offset of ULocalPlayer::AspectRatioAxisConstraint, once reflection has confirmed it
/// (players/unreal_access.h). Until then the split views fill without changing the FOV rule.
void SetAxisConstraintOffset(std::uint32_t offset) noexcept;

/// Switches the test turn; returns the new state (false when there is no player 2).
bool ToggleTestTurn() noexcept;

/// Switches whether split views fill their halves (on at start); returns the new state.
bool ToggleFill() noexcept;

/// True if `localPlayer`'s vtable is the engine's ULocalPlayer vtable, i.e. its view point goes
/// through the installed entry. A different vtable (a subclass) would bypass the turn.
bool UsesEngineViewPoint(const void* localPlayer) noexcept;

}  // namespace lss::views
