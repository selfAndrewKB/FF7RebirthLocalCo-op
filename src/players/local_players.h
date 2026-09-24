#pragma once

// Player 2 joins and leaves through UE's own local-player path.
//
// Joining adds a ULocalPlayer with the engine's CreatePlayer. The engine then lays out the split
// screen by itself, and the renderer module renders each player's view as its own family.
// Leaving removes that ULocalPlayer with the engine's RemoveLocalPlayer.
//
// Every call here must run on the game thread, outside Draw: the mod calls them from UE4SS's
// engine-tick and map-load callbacks (SPLITSCREEN-ARCHITECTURE.md §2.3).

#include "players/shared_camera.h"
#include "players/unreal_access.h"

#include <cstdint>

namespace lss::players {

class LocalPlayers
{
public:
    /// `removeLocalPlayer` is the live address of UGameInstance::RemoveLocalPlayer, verified at
    /// startup against engine::kRemoveLocalPlayer's bytes.
    explicit LocalPlayers(std::uintptr_t removeLocalPlayer) noexcept : removeLocalPlayer_(removeLocalPlayer) {}

    /// Adds player 2, if there is exactly one local player and it has a controller.
    void Join() noexcept;

    /// Removes player 2, if this mod added it. Returns true when player 2 is out (or was never
    /// in), false when it is still in the engine's list.
    bool Leave() noexcept;

    /// Per-tick upkeep while player 2 is in: keeps the shared camera on player 1's controller.
    void Tick() noexcept;

    bool HasPlayerTwo() const noexcept { return playerTwo_ != nullptr; }

private:
    /// True if the engine still lists player 2. If not, lets go of it without touching its object.
    bool StillListed() noexcept;

    /// Drops this mod's references to player 2 once the engine no longer lists it.
    void Forget() noexcept;

    std::uintptr_t   removeLocalPlayer_;
    unreal::UObject* playerTwo_    = nullptr;  // only ever a player this mod created
    unreal::UObject* gameInstance_ = nullptr;  // the game instance whose list player 2 was added to
    SharedCamera     camera_;
};

}  // namespace lss::players
