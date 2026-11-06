#pragma once

// Each player's camera plays only its own character's animation camera overrides
// (RE-p2-battle-camera.md §5, pass S-CAM4).
//
// Attack animations can carry an "override camera" notify state (UEndAnimNotifyStateOverrideCameraParams).
// It sends a request through a delegate on the character, bound by the camera manager that follows
// that character, into the NotifyOverride camera module (layer 4). That module is one global
// instance (g_NotifyOverrideCameraModuleWeak): the first camera operator built creates it and every
// later operator of every camera manager shares it, and it frames around its owner operator's
// character, player 1's. So a seat's attacks drove player 1's camera, and that
// seat's camera played them framed around player 1.
//
// Step A: while a seat's camera manager's BeginPlay runs, the global is hidden, so the game's own
// operator constructor builds that seat's camera a module of its own, owned by its Battle operator;
// the global is then put back. Step B: each frame, the seat's member's two override delegates
// (added / removed) point at thunks that run the game's own handlers with the global holding the
// seat's module and the seat's player as the primary controller, then put it back. The thunks find
// the seat from the delegate's payload (the member). Both are put back when the seat's player leaves.
// The global, both cameras' layer-4 modules and both characters' delegates are logged on change
// (player 2's, a dev tool).
//
// Per seat (players/seats.h): the module and the member holding the thunks. Shared: the global, swapped
// in turn (one engine call at a time). Game thread only.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::camera::camera_override {

bool Install(const platform::GameImage& image) noexcept;

/// Runs the engine's camera-manager BeginPlay for the camera manager a join creates (`seat`: the seat joining, as
/// session/own_controller's join scope names it) so that it builds its own NotifyOverride module (Step A). A local
/// player that is no built seat's (the mirror view, views/mirror_player.h) gets the same hidden global and the module
/// its operators built is released: it plays no character, and player 1's global stays player 1's.
void BeginPlayWithOwnModule(int seat, void (*engineBeginPlay)(void*), void* cameraManager);

/// Once per frame while `seat`'s player plays: the seat's member's delegates on the thunks (Step B); logs.
void Tick(int seat, void* member) noexcept;

/// `seat`'s player is leaving: the member's delegates back to the engine's handlers, the seat's module
/// released.
void SeatOut(int seat) noexcept;

/// `seat`'s member was destroyed (a retry or checkpoint reload rebuilt the party): forgotten, with
/// nothing written to it.
void MemberDestroyed(int seat) noexcept;

}  // namespace lss::camera::camera_override
