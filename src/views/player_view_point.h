#pragma once

// Each local player's view point: its shape while the screen is split, and which camera player 2's
// half shows.
//
// The mod replaces the ULocalPlayer vtable entry for GetViewPoint (engine_addresses.h) and calls
// the engine's function. With one player the result is exactly the engine's. While player 2 is in:
//   - every player's view fills its half: the game's 16:9 constraint is cleared and the FOV rule
//     becomes MajorAxisFOV (views/view_shape.h); Ctrl+F4 switches this off to compare with bars.
//     Each player's own FOV rule is put back when player 2 leaves.
//   - player 2's half shows player 1's camera until Ctrl+F6 switches it to player 2's own camera
//     (and back). Starting on player 1's means a player-2 camera that is not set up yet (it sits
//     at the world origin until it has a character) is never on screen by default.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views {

/// Installs the view-point entry. Call once, after every engine fact has been verified.
/// Returns false, changing nothing, if the entry does not hold the engine's function.
bool InstallPlayerViewPoint(const platform::GameImage& image) noexcept;

/// Puts the engine's own function back (at mod uninstall).
void RemovePlayerViewPoint() noexcept;

/// Player 1's and player 2's ULocalPlayer (player 2 null when there is none). Game thread. A new
/// player 2 starts on player 1's camera, the FOV rule of every remaining player the mod changed is
/// put back, and the next view point of the new player 2 is described in the log.
void SetPlayers(const void* playerOne, const void* playerTwo) noexcept;

/// The offset of ULocalPlayer::AspectRatioAxisConstraint, once reflection has confirmed it
/// (players/unreal_access.h). Until then the split views fill without changing the FOV rule.
void SetAxisConstraintOffset(std::uint32_t offset) noexcept;

/// Switches player 2's half between player 1's camera and player 2's own; returns true when it
/// now shows player 2's own (false, and player 1's, when there is no player 2).
bool ToggleOwnCamera() noexcept;

/// Switches whether split views fill their halves (on at start); returns the new state.
bool ToggleFill() noexcept;

/// True if `localPlayer`'s vtable is the engine's ULocalPlayer vtable, i.e. its view point goes
/// through the installed entry. A different vtable (a subclass) would bypass the camera choice.
bool UsesEngineViewPoint(const void* localPlayer) noexcept;

}  // namespace lss::views
