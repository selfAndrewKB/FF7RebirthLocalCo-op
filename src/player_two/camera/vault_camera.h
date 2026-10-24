#pragma once

// Player 2's chocobo vaulting off a ledge no longer takes both players' ride framing
// (research/RE-p2-chocobo-jump-camera.md, lever L1b).
//
// A vault (control state 10) pushes FEndBehaviorVaultExtraAction. Its Begin, on the game thread, installs an
// FEndVaultCameraModule in slot 15 of the primary camera manager's Field operator when IsUCPC_Core(character) says
// yes; its End, on the animation worker thread, deactivates whatever module is in the primary camera's slot 15. For a
// ridden chocobo the game's own answer is no (player 1's chocobo never gets a vault camera). Player 2's chocobo said
// yes only because the mod's ride tick answers the leader test yes for that chocobo's input component: so the vault
// camera replaced player 2's ride module on PCM2, and the End, outside every scope, deactivated player 1's ride module
// on PCM1 (live 2026-10-07 18:26).
//
// The vault Begin's call of IsUCPC_Core runs the game's test with that component answer held off
// (member_movement::WithoutChocoboAnswer): player 2's chocobo answers no, as player 1's does, so no vault camera is
// installed and the End finds its flag clear. Everything else in the ride tick keeps the answer. Each vault by player 2's
// chocobo or member is logged (Info) with the test's answer; on foot the member keeps the game's yes inside the
// member's tick, so the vault camera goes on player 2's camera (live 2026-10-07 22:20), as player 1's goes on his.
//
// Lever L2: the End's `call PrimaryPCM_FieldOp_DeactivateModule(15)` goes through a stub that passes the action (rbx).
// When the action's character (its weak value, compared, never resolved off the game thread) is a seat's member or
// chocobo, the game's FEndModularCameraOperator::DeactivateModule runs on that seat's Field operator instead; every
// other vault ends as the game ends it. Without it, player 2's vault camera stayed active on player 2's camera (live
// 22:20:06) and the End deactivated player 1's slot 15.
// Per seat (players/seats.h): the cached Field operator; the seat comes from the vaulting character (its member, or
// its chocobo: chocobo_ride's, player 2's in the Begin's test until the rides are per seat).
// Install, Remove and Tick: game thread. The End's handler: any thread (the animation worker in practice).

#include "platform/image.h"

namespace lss::player_two::vault_camera {

/// Redirects the vault Begin's IsUCPC_Core call (checked call and callee; logs the outcome).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Keeps each seat's Field operator for the vault's End on the animation worker (the members' and chocobos' weak
/// values are generation's). Game thread, every engine tick.
void Tick() noexcept;

}  // namespace lss::player_two::vault_camera
