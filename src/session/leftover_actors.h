#pragma once

// The actors a seat's objects leave behind, destroyed by exact pointer once its player is removed.
//
// Removing a local player destroys its controller, and the controller destroys its camera manager,
// but nothing destroys these (the engine's own EndPlay/Destroyed of these classes do not):
//   - the trigger actor made for the party-member switch (controller +0x610);
//   - the battle-talk actor the controller spawns at BeginPlay (controller +0x5B0);
//   - the camera actor the camera manager spawns (camera manager +0x2DE8);
//   - the collision actor the camera manager spawns, found as the owner (outer) of the collision
//     component the camera manager keeps (+0x2D48).
// Collect reads them while the controller and camera manager are still live; the caller removes
// the seat's player, and only if that worked calls Destroy (AActor::K2_DestroyActor on each) in the same
// game-thread step. Destroying first would leave a camera manager that is still updating with a
// destroyed camera actor, which its update reads without a null check, if the removal then failed.
//
// The controller's PlayerInput (+0x350) is not an actor but is left behind too, and it keeps the
// controller alive (RE-p2-controller-after-leave.md, S-PC2LEAK2): APlayerController::Destroyed only
// nulls the field, while the key-map cache of every level script's input component (stock UE4) still
// names it, and its outer is the controller. Destroy marks it PendingKill, as DestroyActor marks the
// controller's own components; the collector then nulls the cache entries, the input components drop
// them, and the controller and the PlayerInput are freed.

namespace lss::session::leftover_actors {

struct Actors
{
    void* trigger           = nullptr;
    void* battleTalk        = nullptr;
    void* cameraActor       = nullptr;
    void* collisionActor    = nullptr;
    void* playerInput       = nullptr;  // not an actor: marked PendingKill
};

/// Reads the seat's leftover actors and PlayerInput. Game thread.
Actors Collect(void* seatController) noexcept;

/// How many actors were destroyed, how many calls raised an exception, and whether the PlayerInput
/// was marked PendingKill.
struct Result
{
    int  destroyed     = 0;
    int  failed        = 0;
    bool inputReleased = false;
};

/// Destroys every actor in `actors` and marks the PlayerInput PendingKill. Game thread, in the same
/// step as (after) the removal of the seat's player.
Result Destroy(const Actors& actors) noexcept;

}  // namespace lss::session::leftover_actors
