#include "players/local_players.h"

#include "platform/log.h"

namespace lss::players {

namespace {

constexpr std::int32_t kPlayerTwoControllerId = 1;

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
    unreal::UObject*  playerOne      = (*players)[0];
    unreal::UObject** oneController  = unreal::ControllerField(playerOne);
    if (oneController == nullptr || *oneController == nullptr)
    {
        platform::Log(L"join refused: player 1 has no controller yet");
        return;
    }

    // The engine appends the new local player to the same array.
    if (!unreal::CreatePlayerWithoutController(*oneController, kPlayerTwoControllerId) || players->Num() != 2)
    {
        platform::Log(L"join failed: the engine did not add a local player (count now {})", players->Num());
        return;
    }
    unreal::UObject* playerTwo = (*players)[1];

    if (!camera_.Attach(playerOne, playerTwo))
    {
        // Without a controller player 2 has no view, and its half would stay black. Take it
        // back out; with an empty controller field that removal touches no controller.
        platform::Log(L"join failed: player 2 could not share player 1's camera; removing it again");
        if (!RemoveFromEngine(gameInstance, playerTwo))
        {
            // Keep tracking it, so Leave (and the leave before every map load) retries the
            // removal. An untracked, controller-less player would reach the engine's map-load
            // path, which runs the faulting login for it.
            playerTwo_ = playerTwo;
            platform::Log(L"player 2 could not be removed again; Ctrl+F8 retries, and it is retried before any map load");
        }
        return;
    }

    playerTwo_ = playerTwo;
    platform::Log(L"player 2 joined (sharing player 1's camera for now)");
}

bool LocalPlayers::Leave() noexcept
{
    if (!HasPlayerTwo())
    {
        return true;
    }

    // Order matters: RemoveLocalPlayer destroys the controller in the player's field. Until the
    // loan is cleared, that is player 1's controller (RE-remove-local-player.md §Q1).
    if (!camera_.Detach())
    {
        platform::Log(L"leave refused: player 2 holds a controller this mod did not lend; removing "
                      L"player 2 would destroy it");
        return false;
    }

    if (!RemoveFromEngine(unreal::GameInstance(), playerTwo_))
    {
        // Player 2 stays, without a camera (its half is black). Leaving can be tried again.
        platform::Log(L"leave failed: the engine did not remove player 2");
        return false;
    }
    playerTwo_ = nullptr;  // the engine's garbage collector frees it; this mod must not touch it again
    platform::Log(L"player 2 left");
    return true;
}

bool LocalPlayers::RemoveFromEngine(unreal::UObject* gameInstance, unreal::UObject* localPlayer) noexcept
{
    auto* players = unreal::LocalPlayers(gameInstance);
    if (players == nullptr || !unreal::RemoveLocalPlayer(gameInstance, localPlayer, removeLocalPlayer_))
    {
        return false;
    }
    // RemoveLocalPlayer reports success even when the player was not in the list, so the list
    // itself is the evidence.
    for (std::int32_t i = 0; i < players->Num(); ++i)
    {
        if ((*players)[i] == localPlayer)
        {
            return false;
        }
    }
    return true;
}

}  // namespace lss::players
