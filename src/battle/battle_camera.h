#pragma once

// Player 2's battle camera (RE-p2-battle-camera.md).
//
// Each End camera manager owns 13 camera operators (EEndCameraOperatorType); each frame its update
// renders the lowest-numbered active one, so the Battle operator (11, +0x2CA8) takes over from the
// Field operator (12) while it is on. The battle state machine switches the Battle operator on at
// battle start and off at battle end, but through "the primary camera manager": player 1's only. So
// player 2's camera stayed on the Field operator in battle.
//
// As camera/field_camera does for the Field operator, the Battle operator's Activate and
// Deactivate (vtable +0x10 / +0x20) are replaced: after the engine's call on player 1's operator, the
// same call runs on player 2's Battle operator, with player 2 as the primary controller (Activate
// reads the primary camera manager's pose). The Battle operator then switches on its own modules
// (the battle offset and length) for player 2's character, which player 2's camera already follows.
// Seed brings player 2's operator to player 1's state when player 2 takes a member mid-battle.
// Watch logs both camera managers' current operator and both Battle operators' state on change.
// Game thread only.
//
// Per seat: every built seat's Battle operator follows player 1's switches, in seat order, each call with that seat as
// the primary controller (one scope at a time). Seed takes the seat; Watch is a dev tool and stays player 2's.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_camera {

/// Replaces both Battle-operator slots, or neither. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// `seat`'s Battle operator to player 1's active state. Game thread, after the seat took a member.
bool Seed(int seat) noexcept;

/// Whether `cameraManager`'s Battle operator is on (a seat's camera update may then run as the primary player's, as
/// it does with the Field operator on). Whichever seat's camera manager it is.
bool SeatBattleActive(void* cameraManager) noexcept;

/// Logs, on change, both camera managers' operators (game thread, once per frame).
void Watch() noexcept;

}  // namespace lss::battle::battle_camera
