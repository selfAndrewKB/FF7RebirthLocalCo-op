#pragma once

// A local player of the mod's, added to and removed from the engine's list of local players through UE's own path,
// by seat (players/seats.h): seat 1 is player 2 (player_two/session/session.h), seat 2 the third view
// (views/third_view.h). The controller id is the seat. The engine appends the player to its list, after whoever is
// listed, and closes the list up when one leaves, so the list position is never taken for the seat: the new player
// is the one appended, and a seat is free when no listed player has its controller id.
//
// Adding uses the engine's CreatePlayer with a PlayerController of the player's own. The engine then
// lays out the split screen by itself, and the renderer module renders each player's view as its
// own family. Removing uses the engine's RemoveLocalPlayer, which also destroys the player's
// controller and, through it, its camera manager.
//
// This class only keeps the engine's list and its own record of the player in step. What the player
// does while it is in is its owner's job. Every call must run on the game thread, outside Draw.

#include "players/unreal_access.h"

#include <cstdint>

namespace lss::players {

/// The controller id the engine gave `localPlayer` (ULocalPlayer::ControllerId), which for the mod's players is the
/// seat; -1 if unreadable (0 is player 1's, so a failed read must not give it).
std::int32_t LocalPlayerControllerId(const unreal::UObject* localPlayer) noexcept;

class LocalPlayers
{
public:
    /// `removeLocalPlayer` is the live address of UGameInstance::RemoveLocalPlayer, verified at
    /// startup against engine::kRemoveLocalPlayer's bytes. `seat`: the player's controller id (1 = player 2).
    explicit LocalPlayers(std::uintptr_t removeLocalPlayer, std::int32_t seat = 1) noexcept
        : removeLocalPlayer_(removeLocalPlayer), seat_(seat)
    {
    }

    /// What an add did.
    struct Added
    {
        bool             raised     = false;    // the engine raised an exception during the add
        unreal::UObject* controller = nullptr;  // the player's controller as the engine lists it
    };

    /// Player 1's local player and controller, when the list has room for one more (1 to kMaxPlayers - 1 listed),
    /// no listed player has the seat's controller id, and player 1 has a controller; otherwise both null (and the
    /// reason is logged when `log` is set). A caller that needs another seat in first checks that itself (the third
    /// view: player 2's generation).
    struct PlayerOne
    {
        unreal::UObject* localPlayer = nullptr;
        unreal::UObject* controller  = nullptr;
    };
    PlayerOne FirstPlayer(bool log = true) const noexcept;

    /// Adds the player with its own controller. Call only with FirstPlayer() valid. Any player the
    /// engine added is tracked, even when the call raised an exception, so it is removed before a
    /// map load.
    Added Add(unreal::UObject* playerOneController) noexcept;

    /// Removes the player (its controller is destroyed with it). True when the player is out.
    bool Remove() noexcept;

    /// True if the engine still lists the player. If not, lets go of it without touching its object.
    bool StillListed() noexcept;

    bool HasPlayer() const noexcept { return player_ != nullptr; }

    /// The player's ULocalPlayer, or null. Only compared by address outside this class.
    unreal::UObject* Player() const noexcept { return player_; }

    std::int32_t Seat() const noexcept { return seat_; }

    /// Counts adds. A new player can be allocated at the address of the one that left, so
    /// observers tell joins apart by this, not by the address alone.
    std::uint32_t JoinCount() const noexcept { return joins_; }

private:
    void Forget() noexcept;

    std::uintptr_t   removeLocalPlayer_;
    std::int32_t     seat_;
    unreal::UObject* player_       = nullptr;  // only ever a player this object created
    unreal::UObject* gameInstance_ = nullptr;  // the game instance whose list the player was added to
    std::uint32_t    joins_        = 0;
};

}  // namespace lss::players
