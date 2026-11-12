#pragma once

// Each seat's member's own battle camera effects on that seat's camera (players/seats.h;
// research/RE-p2-attack-cameras.md).
//
// Three game paths play a camera effect for "the leader" on "the primary camera" (GetEndPlayerCameraManager, the
// party manager's primary controller). For a seat's member they ran with player 1 as both, so:
//   - the guard camera effect (the battle-player component's PlayCameraEffectIfEnabled, gated on the leader test)
//     shook player 1's camera when a seat's member guarded;
//   - the camera-shake notifies of the member's montages (UEndBattleCameraShakeNotify / ...NotifyState, gated on the
//     leader test) never played at all;
//   - StealCamera (chain / synergy moves: "the leader's camera looks between the leader and the acting partner")
//     only ever pulled player 1's camera.
// Each wrapper finds the seat from what the game hands it (the component's player type, the mesh's character, the
// pair's member) and runs the game's own function in that seat's leader view, so the game's own gate decides for the
// seat and the effect lands on the seat's camera:
//   - guard effect and shakes owned by a seat's member: leader, leader type and primary controller scoped (the
//     NotifyState's End too, since it stops the shake by a per-component handle on the primary camera);
//   - StealCamera Begin owned by another party character that shares a seat's member's action key (a chain pair with
//     that member): leader and leader type scoped, so the request lands on the seat's member, whose delegate the
//     seat's Battle operator holds; a seat's member's own notify stays the game's (it pulls player 1's camera toward
//     that member, the solo look for player 1). End reads no leader.
// Everything else calls the game's function unchanged. Per seat: the counts and the remembered shakes (with their
// seat). Game thread only: a call on any other thread passes through unscoped (counted).

namespace lss::platform {
class GameImage;
}

namespace lss::camera::seat_camera_effects {

/// Replaces the slots; each item installs whole or stays off (logged).
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out).
void Remove() noexcept;

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::camera::seat_camera_effects
