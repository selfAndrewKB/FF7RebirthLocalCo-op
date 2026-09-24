#pragma once

// Each local player's view point, with player 2's optionally turned away from player 1's.
//
// Player 2 has no camera of its own yet: it borrows player 1's controller, so the engine gives it
// player 1's view. For the crash test (SPLITSCREEN-ARCHITECTURE.md §9) player 2's view can be
// turned 30° to the side, a test instrument that stands in for a real second camera. The mod
// replaces the ULocalPlayer vtable entry for GetViewPoint (engine_addresses.h), calls the
// engine's function, and turns the result only for player 2 while the turn is on. Every other
// player, and player 2 with the turn off, gets exactly what the engine computed.

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
/// test turn off, and the next view point of the new player 2 is described in the log.
void SetPlayerTwo(const void* localPlayer) noexcept;

/// Switches the test turn; returns the new state (false when there is no player 2).
bool ToggleTestTurn() noexcept;

/// True if `localPlayer`'s vtable is the engine's ULocalPlayer vtable, i.e. its view point goes
/// through the installed entry. A different vtable (a subclass) would bypass the turn.
bool UsesEngineViewPoint(const void* localPlayer) noexcept;

}  // namespace lss::views
