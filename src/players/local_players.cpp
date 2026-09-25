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

LocalPlayers::PlayerOne LocalPlayers::SolePlayer() const noexcept
{
    auto* players = unreal::LocalPlayers(unreal::GameInstance());
    if (players == nullptr || players->Num() != 1)
    {
        platform::Log(L"join refused: expected exactly one local player, found {}", players == nullptr ? -1 : players->Num());
        return {};
    }
    unreal::UObject*  localPlayer = (*players)[0];
    unreal::UObject** controller  = unreal::ControllerField(localPlayer);
    if (controller == nullptr || *controller == nullptr)
    {
        platform::Log(L"join refused: player 1 has no controller yet");
        return {};
    }
    return {localPlayer, *controller};
}

LocalPlayers::Added LocalPlayers::Add(unreal::UObject* playerOneController) noexcept
{
    Added added;
    if (HasPlayerTwo())
    {
        platform::Log(L"player 2 is already in");
        return added;
    }
    unreal::UObject* gameInstance = unreal::GameInstance();
    auto*            players      = unreal::LocalPlayers(gameInstance);
    if (players == nullptr)
    {
        return added;
    }

    const auto created = unreal::CreatePlayerWithController(playerOneController, kPlayerTwoControllerId);
    added.raised       = !created.called;

    // The engine's list decides what happened, not the call: an exception can be raised after the
    // engine already added the player, and such a player must still be removed before a map load.
    if (players->Num() != 2)
    {
        platform::Log(L"join failed: the engine did not add a local player ({}; count now {})",
                      created.called ? L"the call returned" : L"the call raised an exception", players->Num());
        return added;
    }
    playerTwo_    = (*players)[1];  // the engine appends the new player
    gameInstance_ = gameInstance;
    ++joins_;

    unreal::UObject** field = unreal::ControllerField(playerTwo_);
    added.controller        = field == nullptr ? nullptr : *field;
    if (created.called && created.controller != added.controller)
    {
        platform::Log(L"WARNING: CreatePlayer returned a controller other than the one player 2 lists");
    }
    return added;
}

bool LocalPlayers::Remove() noexcept
{
    if (!HasPlayerTwo() || !StillListed())
    {
        return true;
    }
    auto* players = unreal::LocalPlayers(gameInstance_);

    // RemoveLocalPlayer reports success even for a player it did not hold, so membership before
    // (checked above) and after (below) is the evidence.
    if (!unreal::RemoveLocalPlayer(gameInstance_, playerTwo_, removeLocalPlayer_) || Contains(*players, playerTwo_))
    {
        platform::Log(L"leave failed: the engine did not remove player 2");
        return false;
    }
    Forget();
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
    Forget();
    return false;
}

void LocalPlayers::Forget() noexcept
{
    // The engine's garbage collector frees the player; this mod must not touch it again.
    playerTwo_    = nullptr;
    gameInstance_ = nullptr;
}

}  // namespace lss::players
