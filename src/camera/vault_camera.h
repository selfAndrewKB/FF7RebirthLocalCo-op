#pragma once

// A seat's chocobo vaulting off a ledge no longer takes that seat's and player 1's ride framing
// (research/RE-p2-chocobo-jump-camera.md, lever L1b).
//
// A vault (control state 10) pushes FEndBehaviorVaultExtraAction. Its Begin, on the game thread, installs an
// FEndVaultCameraModule in slot 15 of the primary camera manager's Field operator when IsUCPC_Core(character) says
// yes; its End, on the animation worker thread, deactivates whatever module is in the primary camera's slot 15. For a
// ridden chocobo the game's own answer is no (player 1's chocobo never gets a vault camera). A seat's chocobo said
// yes only because the mod's ride tick answers the leader test yes for that chocobo's input component: so the vault
// camera replaced the seat's own ride module, and the End, outside every scope, deactivated player 1's ride module
// on PCM1 (seen live).
//
// The vault Begin's call of IsUCPC_Core runs the game's test with that component answer held off
// (input::member_movement::WithoutChocoboAnswer): a seat's chocobo answers no, as player 1's does, so no vault camera is
// installed and the End finds its flag clear. Everything else in the ride tick keeps the answer. Each vault by a seat's
// chocobo or member is logged (Info) with the test's answer; on foot the member keeps the game's yes inside the
// member's tick, so the vault camera goes on the seat's camera (seen live), as player 1's goes on his.
//
// Lever L2: the End's `call PrimaryPCM_FieldOp_DeactivateModule(15)` goes through a stub that passes the action (rbx).
// When the action's character (its weak value, compared, never resolved off the game thread) is a seat's member or
// chocobo, the game's FEndModularCameraOperator::DeactivateModule runs on that seat's Field operator instead; every
// other vault ends as the game ends it. Without it, a seat's vault camera stayed active on the seat's camera and the
// End deactivated player 1's slot 15.
//
// The chained vault: a vault the game starts straight after another (a cart's second step) is begun on the game
// thread outside the member's tick, so for a seat's member the test answered no and that step ran on the follow
// camera, pulled into the ledge, while player 1's chained vault gets the vault camera. The vault's Begin slot runs a
// seat's member's Begin begun outside the seat's scope inside it (input::member_movement::InPrimaryScope), as the
// member's own vaults run: the camera goes on the seat's camera and L2 ends it there. Placed only with L2.
// Per seat (players/seats.h): the cached Field operator; the seat comes from the vaulting character (its member, or
// its chocobo: chocobo_ride::SeatDriving).
// Install, Remove and Tick: game thread. The End's handler: any thread (the animation worker in practice).

#include "platform/image.h"

namespace lss::camera::vault_camera {

/// Redirects the vault Begin's IsUCPC_Core call and the End's deactivate, and replaces the Begin slot (checked calls,
/// callees and slot; logs each outcome).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Keeps each seat's Field operator for the vault's End on the animation worker (the members' and chocobos' weak
/// values are generation's). Game thread, every engine tick.
void Tick() noexcept;

}  // namespace lss::camera::vault_camera
