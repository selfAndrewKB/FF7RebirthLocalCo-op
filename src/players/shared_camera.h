#pragma once

// STAND-IN, to be replaced when players get their own controllers: player 2 views the world
// through player 1's PlayerController, so both halves show player 1's camera.
//
// Why it exists: the engine only builds a view for a local player that has a PlayerController
// (ULocalPlayer::GetProjectionData refuses otherwise), and spawning a real controller for
// player 2 runs the GameMode login path, which faults in FF7R. Lending player 1's controller is
// how split-screen works today. Nothing else in the mod depends on it.
//
// The loan is one pointer in player 2's `PlayerController` field. It is kept in step with player
// 1 every tick, and cleared before player 2 is removed: RemoveLocalPlayer destroys whatever
// controller that field holds, which would otherwise be player 1's (RE-remove-local-player.md).
//
// This class works only on the two fields' addresses, which the caller finds by reflection, so it
// has no engine or UE4SS dependency and is tested on the host (tests/shared_camera_test.cpp).

namespace RC::Unreal {
class UObject;
}

namespace lss::players {

class SharedCamera
{
public:
    using UObject = RC::Unreal::UObject;

    /// Lends player 1's controller to player 2, given the addresses of both players'
    /// PlayerController fields. Refuses (returns false, writing nothing) if either address is
    /// missing, player 1 has no controller, or player 2's field is already in use.
    bool Attach(UObject** playerOneController, UObject** playerTwoController) noexcept;

    /// Keeps the loan pointing at player 1's current controller (it changes on some transitions),
    /// and restores it if the engine empties player 2's field. Call once per tick while attached.
    /// Returns true if it changed player 2's field.
    bool Refresh() noexcept;

    /// Ends the loan and empties player 2's field, whatever it holds.
    void Detach() noexcept;

    /// Ends the loan without touching either field, for when player 2's object may already be gone.
    void Abandon() noexcept;

    bool IsAttached() const noexcept { return twoField_ != nullptr; }

private:
    UObject** oneField_ = nullptr;  // player 1's PlayerController field
    UObject** twoField_ = nullptr;  // player 2's PlayerController field
    UObject*  lent_     = nullptr;  // the controller currently lent to player 2
};

}  // namespace lss::players
