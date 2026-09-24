#pragma once

// STAND-IN, to be replaced when players get their own controllers (philosophy §4.7):
// player 2 views the world through player 1's PlayerController, so both halves show player 1's
// camera.
//
// Why it exists: the engine only builds a view for a local player that has a PlayerController
// (ULocalPlayer::GetProjectionData refuses otherwise), and spawning a real controller for
// player 2 runs the GameMode login path, which faults in FF7R. Lending player 1's controller is
// how split-screen works today. Nothing else in the mod depends on it.
//
// The loan is one pointer in player 2's `PlayerController` field. It is kept in step with player
// 1 every tick, and cleared before player 2 is removed: RemoveLocalPlayer destroys whatever
// controller that field holds, which would otherwise be player 1's (RE-remove-local-player.md).

#include "players/unreal_access.h"

namespace lss::players {

class SharedCamera
{
public:
    /// Lends player 1's controller to player 2. Refuses (returns false, writing nothing) if either
    /// field cannot be found, player 1 has no controller, or player 2's field is already in use.
    bool Attach(unreal::UObject* playerOne, unreal::UObject* playerTwo) noexcept;

    /// Keeps the loan pointing at player 1's current controller (it changes on some transitions).
    /// Call once per tick while attached.
    void Refresh() noexcept;

    /// Clears player 2's field if it still holds our loan. Returns true when the field is empty
    /// afterwards, which is the condition for removing player 2 safely.
    bool Detach() noexcept;

    bool IsAttached() const noexcept { return twoField_ != nullptr; }

private:
    unreal::UObject** oneField_ = nullptr;  // player 1's PlayerController field
    unreal::UObject** twoField_ = nullptr;  // player 2's PlayerController field
    unreal::UObject*  lent_     = nullptr;  // the controller currently lent to player 2
};

}  // namespace lss::players
