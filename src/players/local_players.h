#pragma once

// Player 2 is added to and removed from the engine's list of local players through UE's own path.
//
// Adding uses the engine's CreatePlayer with a PlayerController of player 2's own. The engine then
// lays out the split screen by itself, and the renderer module renders each player's view as its
// own family. Removing uses the engine's RemoveLocalPlayer, which also destroys player 2's
// controller and, through it, its camera manager.
//
// This class only keeps the engine's list and its own record of player 2 in step. What player 2
// does while it is in is player_two/session.h's job. Every call must run on the game thread,
// outside Draw.

#include "players/unreal_access.h"

#include <cstdint>

namespace lss::players {

class LocalPlayers
{
public:
    /// `removeLocalPlayer` is the live address of UGameInstance::RemoveLocalPlayer, verified at
    /// startup against engine::kRemoveLocalPlayer's bytes.
    explicit LocalPlayers(std::uintptr_t removeLocalPlayer) noexcept : removeLocalPlayer_(removeLocalPlayer) {}

    /// What an add did.
    struct Added
    {
        bool             raised     = false;    // the engine raised an exception during the add
        unreal::UObject* controller = nullptr;  // player 2's controller as the engine lists it
    };

    /// Player 1's local player and controller, when there is exactly one local player and it has a
    /// controller; otherwise both null (and the reason is logged).
    struct PlayerOne
    {
        unreal::UObject* localPlayer = nullptr;
        unreal::UObject* controller  = nullptr;
    };
    PlayerOne SolePlayer() const noexcept;

    /// Adds player 2 with its own controller. Call only with SolePlayer() valid. Any player the
    /// engine added is tracked, even when the call raised an exception, so it is removed before a
    /// map load.
    Added Add(unreal::UObject* playerOneController) noexcept;

    /// Removes player 2 (its controller is destroyed with it). True when player 2 is out.
    bool Remove() noexcept;

    /// True if the engine still lists player 2. If not, lets go of it without touching its object.
    bool StillListed() noexcept;

    bool HasPlayerTwo() const noexcept { return playerTwo_ != nullptr; }

    /// Player 2's ULocalPlayer, or null. Only compared by address outside this class.
    unreal::UObject* PlayerTwo() const noexcept { return playerTwo_; }

    /// Counts adds. A new player 2 can be allocated at the address of the one that left, so
    /// observers tell joins apart by this, not by the address alone.
    std::uint32_t JoinCount() const noexcept { return joins_; }

private:
    void Forget() noexcept;

    std::uintptr_t   removeLocalPlayer_;
    unreal::UObject* playerTwo_    = nullptr;  // only ever a player this mod created
    unreal::UObject* gameInstance_ = nullptr;  // the game instance whose list player 2 was added to
    std::uint32_t    joins_        = 0;
};

}  // namespace lss::players
