#include "players/local_players.h"

#include "platform/log.h"

namespace lss::players {

namespace {

constexpr std::int32_t kPlayerTwoControllerId = 1;

bool Contains(RC::Unreal::TArray<unreal::UObject*>& players, unreal::UObject* player) noexcept
{
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        if (players[i] == player)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

void LocalPlayers::Join() noexcept
{
    if (HasPlayerTwo())
    {
        platform::Log(L"player 2 is already in");
        return;
    }

    unreal::UObject* gameInstance = unreal::GameInstance();
    auto*            players      = unreal::LocalPlayers(gameInstance);
    if (players == nullptr || players->Num() != 1)
    {
        platform::Log(L"join refused: expected exactly one local player, found {}",
                      players == nullptr ? -1 : players->Num());
        return;
    }
    unreal::UObject*  playerOne     = (*players)[0];
    unreal::UObject** oneController = unreal::ControllerField(playerOne);
    if (oneController == nullptr || *oneController == nullptr)
    {
        platform::Log(L"join refused: player 1 has no controller yet");
        return;
    }

    const bool called = unreal::CreatePlayerWithoutController(*oneController, kPlayerTwoControllerId);

    // The engine's list decides what happened, not the call: an exception can be raised after the
    // engine already added the player. Any player that now exists must be tracked, because an
    // untracked controller-less player would reach the engine's map-load login, which faults.
    if (players->Num() != 2)
    {
        platform::Log(L"join failed: the engine did not add a local player ({}; count now {})",
                      called ? L"the call returned" : L"the call raised an exception", players->Num());
        return;
    }
    playerTwo_    = (*players)[1];  // the engine appends the new player
    gameInstance_ = gameInstance;
    if (!called)
    {
        platform::Log(L"CreatePlayer raised an exception but player 2 was added; continuing with it");
    }

    if (!camera_.Attach(oneController, unreal::ControllerField(playerTwo_)))
    {
        // Without a controller player 2 has no view, and its half would stay black. Take it back
        // out; with an empty controller field the removal touches no controller.
        platform::Log(L"join failed: player 2 could not share player 1's camera; removing it again");
        if (!Leave())
        {
            platform::Log(L"player 2 could not be removed again; Ctrl+F8 retries, and it is retried before any map load");
        }
        return;
    }
    ++joins_;
    platform::Log(L"player 2 joined (sharing player 1's camera for now)");
}

bool LocalPlayers::Leave() noexcept
{
    if (!HasPlayerTwo())
    {
        return true;
    }

    if (!StillListed())
    {
        platform::Log(L"player 2 is no longer in the engine's list; nothing to remove");
        return true;
    }

    // RemoveLocalPlayer destroys whatever controller is in the player's field
    // (engine::kRemoveLocalPlayerControllerTest). Player 2 never owns one, so anything there is
    // borrowed: player 1's, or a stale loan. The field must be empty before the call, whether or
    // not the loan was attached (a failed join removes player 2 without one), and a field that
    // cannot be found cannot be shown empty, so the removal is refused.
    camera_.Detach();
    unreal::UObject** twoController = unreal::ControllerField(playerTwo_);
    if (twoController == nullptr)
    {
        platform::Log(L"leave refused: player 2's controller field cannot be found, so it cannot be emptied first");
        return false;
    }
    *twoController = nullptr;

    auto* players = unreal::LocalPlayers(gameInstance_);

    // RemoveLocalPlayer reports success even for a player it did not hold, so membership before
    // (checked above) and after (below) is the evidence.
    if (!unreal::RemoveLocalPlayer(gameInstance_, playerTwo_, removeLocalPlayer_) || Contains(*players, playerTwo_))
    {
        // Player 2 stays, without a camera (its half is black). Leaving can be tried again.
        platform::Log(L"leave failed: the engine did not remove player 2");
        return false;
    }
    Forget();
    platform::Log(L"player 2 left");
    return true;
}

bool LocalPlayers::StillListed() noexcept
{
    // Reads only the engine's list, never player 2's object: if the engine removed player 2 on
    // its own, the object may already have been freed.
    auto* players = unreal::LocalPlayers(gameInstance_);
    if (players != nullptr && Contains(*players, playerTwo_))
    {
        return true;
    }
    camera_.Abandon();
    Forget();
    return false;
}

void LocalPlayers::Tick() noexcept
{
    if (!HasPlayerTwo())
    {
        return;
    }
    if (!StillListed())
    {
        platform::Log(L"player 2 was removed by the game; this mod has let go of it");
        return;
    }
    if (camera_.Refresh())
    {
        platform::Log(L"shared camera re-attached to player 1's current controller");
    }
}

void LocalPlayers::Forget() noexcept
{
    // The engine's garbage collector frees the player; this mod must not touch it again.
    playerTwo_    = nullptr;
    gameInstance_ = nullptr;
}

}  // namespace lss::players
