#pragma once

// Player 2's member's own battle camera effects on player 2's camera (research/RE-p2-attack-cameras.md §2, §9.4-§9.6).
//
// Three game paths play a camera effect for "the leader" on "the primary camera" (GetEndPlayerCameraManager, the
// party manager's primary controller). For player 2's member they ran with player 1 as both, so:
//   - the guard camera effect (the battle-player component's PlayCameraEffectIfEnabled, gated on the leader test)
//     shook player 1's camera when player 2's member guarded;
//   - the camera-shake notifies of the member's montages (UEndBattleCameraShakeNotify / ...NotifyState, gated on the
//     leader test) never played at all;
//   - StealCamera (chain / synergy moves: "the leader's camera looks between the leader and the acting partner")
//     only ever pulled player 1's camera.
// Each wrapper runs the game's own function in player 2's leader view, so the game's own gate decides for player 2 and
// the effect lands on player 2's camera:
//   - guard effect and shakes owned by player 2's member: leader, leader type and primary controller scoped (the
//     NotifyState's End too, since it stops the shake by a per-component handle on the primary camera);
//   - StealCamera Begin owned by another party character that shares player 2's member's action key (a chain pair with
//     player 2's member): leader and leader type scoped, so the request lands on player 2's member, whose delegate
//     player 2's Battle operator holds; player 2's member's own notify stays the game's (it pulls player 1's camera
//     toward player 2's member, the solo look for player 1). End reads no leader.
// Everything else calls the game's function unchanged. Game thread only: a call on any other thread passes through
// unscoped (counted).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::camera_effects_two {

/// Replaces the slots; each item installs whole or stays off (logged).
void Install(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::player_two::camera_effects_two
