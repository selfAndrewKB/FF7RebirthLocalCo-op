#pragma once

// The actors player 2's objects leave behind, destroyed by exact pointer once player 2 is removed.
//
// Removing a local player destroys its controller, and the controller destroys its camera manager,
// but nothing destroys these (the engine's own EndPlay/Destroyed of these classes do not):
//   - the trigger actor made for the party-member switch (controller +0x610);
//   - the battle-talk actor the controller spawns at BeginPlay (controller +0x5B0);
//   - the camera actor the camera manager spawns (camera manager +0x2DE8);
//   - the collision actor the camera manager spawns, found as the owner (outer) of the collision
//     component the camera manager keeps (+0x2D48).
// Collect reads them while the controller and camera manager are still live; the caller removes
// player 2, and only if that worked calls Destroy (AActor::K2_DestroyActor on each) in the same
// game-thread step. Destroying first would leave a camera manager that is still updating with a
// destroyed camera actor, which its update reads without a null check, if the removal then failed.

namespace lss::player_two::leftover_actors {

struct Actors
{
    void* trigger           = nullptr;
    void* battleTalk        = nullptr;
    void* cameraActor       = nullptr;
    void* collisionActor    = nullptr;
};

/// Reads player 2's leftover actors. Game thread.
Actors Collect(void* playerTwoController) noexcept;

/// How many actors were destroyed, and how many calls raised an exception.
struct Result
{
    int destroyed = 0;
    int failed    = 0;
};

/// Destroys every actor in `actors`. Game thread, in the same step as (after) player 2's removal.
Result Destroy(const Actors& actors) noexcept;

}  // namespace lss::player_two::leftover_actors
