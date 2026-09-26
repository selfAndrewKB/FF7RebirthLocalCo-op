#pragma once

// Each player's camera plays only its own character's animation camera overrides
// (RE-p2-battle-camera.md §5, pass S-CAM4).
//
// Attack animations can carry an "override camera" notify state (UEndAnimNotifyStateOverrideCameraParams).
// It sends a request through a delegate on the character, bound by the camera manager that follows
// that character, into the NotifyOverride camera module (layer 4). That module is one global
// instance (g_NotifyOverrideCameraModuleWeak): the first camera operator built creates it and every
// later operator of every camera manager shares it, and it frames around its owner operator's
// character, player 1's. So player 2's attacks drove player 1's camera, and player 2's camera
// played them framed around player 1.
//
// Step A: while player 2's camera manager's BeginPlay runs, the global is hidden, so the game's own
// operator constructor builds player 2's camera a module of its own, owned by player 2's Battle
// operator; the global is then put back. Step B: each frame, player 2's member's two override
// delegates (added / removed) point at thunks that run the game's own handlers with the global
// holding player 2's module and player 2 as the primary controller, then put it back. Both are put
// back when player 2 leaves. The global, both cameras' layer-4 modules and both characters'
// delegates are logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::camera_override {

bool Install(const platform::GameImage& image) noexcept;

/// Runs the engine's camera-manager BeginPlay for player 2's camera manager so that it builds its
/// own NotifyOverride module (Step A). Called by player_two/own_controller inside its own wrapper.
void BeginPlayWithOwnModule(void (*engineBeginPlay)(void*), void* cameraManager);

/// Once per frame while player 2 plays: player 2's member's delegates on the thunks (Step B); logs.
void Tick(void* member) noexcept;

/// Player 2 is leaving: the member's delegates back to the engine's handlers, player 2's module
/// released.
void PlayerTwoOut() noexcept;

}  // namespace lss::player_two::camera_override
