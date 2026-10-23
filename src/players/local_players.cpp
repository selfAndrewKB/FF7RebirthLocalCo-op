#include "players/local_players.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/log.h"

#include <Unreal/UFunction.hpp>

namespace lss::players {

namespace {

using unreal::UObject;

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

/// UGameplayStatics::CreatePlayer's parameter frame. Checked against the UFunction's own
/// parameter size before every call, so a different engine layout is refused, not written.
struct CreatePlayerParams
{
    UObject*     WorldContextObject;
    std::int32_t ControllerId;
    bool         bSpawnPlayerController;
    UObject*     ReturnValue;  // the new player's controller: always null without spawning one
};
static_assert(sizeof(CreatePlayerParams) == 24);

using RemoveLocalPlayerFn = bool (*)(UObject* gameInstance, UObject* localPlayer);

// The two engine calls run inside structured-exception guards, so a fault inside the engine is
// reported as a failed call rather than ending the game. FF7R is known to raise one on a
// CreatePlayer path (the login fault that bSpawnPlayerController = true triggers).
//
// The trade-off: a fault part-way through leaves the engine half-changed. That is why every
// caller checks the engine's own list afterwards instead of trusting the call. A stack overflow
// is never caught: the stack is unusable until the guard page is re-armed, so continuing would
// only fault again somewhere less diagnosable.
//
// (platform/guarded.h has the rules for these guards.)

using platform::GuardFilter;

bool GuardedRemove(RemoveLocalPlayerFn remove, UObject* gameInstance, UObject* localPlayer) noexcept
{
    __try
    {
        remove(gameInstance, localPlayer);  // its bool result is always true, so it is not read
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// UGameplayStatics::CreatePlayer(worldContext, controllerId, bSpawnPlayerController = true): a
/// local player with its own PlayerController. The login path's camera-manager BeginPlay faults for
/// a second player unless player_two/session/own_controller.h is installed and a join scope is open.
struct CreatedPlayer
{
    bool     called     = false;  // the function was found and the call returned without an exception
    UObject* controller = nullptr;  // the new player's controller, as the call returned it
};
CreatedPlayer CreatePlayerWithController(UObject* worldContext, std::int32_t controllerId) noexcept
{
    const unreal::StaticCall call =
        unreal::FindStatic(unreal::kGameplayStatics, STR("/Script/Engine.GameplayStatics:CreatePlayer"));
    if (call.function == nullptr || worldContext == nullptr ||
        call.function->GetParmsSize() != sizeof(CreatePlayerParams))
    {
        return {};
    }

    CreatePlayerParams params{};
    params.WorldContextObject     = worldContext;
    params.ControllerId           = controllerId;
    params.bSpawnPlayerController = true;
    const bool called             = unreal::GuardedProcessEvent(call.defaultObject, call.function, &params);
    return {called, called ? params.ReturnValue : nullptr};
}

/// Calls UGameInstance::RemoveLocalPlayer through the game instance's own vtable, after checking
/// that the entry holds the function at `removeLocalPlayer` (a live address). Returns false,
/// calling nothing, if it does not, or if the call raised an exception.
bool RemoveLocalPlayer(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer) noexcept
{
    if (gameInstance == nullptr || localPlayer == nullptr)
    {
        return false;
    }
    // The game instance's class decides which vtable is used; RE found no class overriding this
    // entry, and this check makes that a precondition rather than an assumption.
    const auto vtable = *reinterpret_cast<const std::uintptr_t* const*>(gameInstance);
    const auto entry  = vtable[facts::kRemoveLocalPlayerVTableOffset / sizeof(std::uintptr_t)];
    if (entry != removeLocalPlayer)
    {
        return false;
    }
    return GuardedRemove(reinterpret_cast<RemoveLocalPlayerFn>(entry), gameInstance, localPlayer);
}

}  // namespace

LocalPlayers::PlayerOne LocalPlayers::FirstPlayer(bool log) const noexcept
{
    auto* players = unreal::LocalPlayers(unreal::GameInstance());
    if (players == nullptr || players->Num() != seat_)
    {
        if (log)
        {
            platform::Log(L"join refused: expected exactly {} local player(s), found {}", seat_,
                          players == nullptr ? -1 : players->Num());
        }
        return {};
    }
    unreal::UObject*  localPlayer = (*players)[0];
    unreal::UObject** controller  = unreal::ControllerField(localPlayer);
    if (controller == nullptr || *controller == nullptr)
    {
        if (log)
        {
            platform::Log(L"join refused: player 1 has no controller yet");
        }
        return {};
    }
    return {localPlayer, *controller};
}

LocalPlayers::Added LocalPlayers::Add(unreal::UObject* playerOneController) noexcept
{
    Added added;
    if (HasPlayer())
    {
        platform::Log(L"player {} is already in", seat_ + 1);
        return added;
    }
    unreal::UObject* gameInstance = unreal::GameInstance();
    auto*            players      = unreal::LocalPlayers(gameInstance);
    if (players == nullptr)
    {
        return added;
    }

    const auto created = CreatePlayerWithController(playerOneController, seat_);
    added.raised       = !created.called;

    // The engine's list decides what happened, not the call: an exception can be raised after the
    // engine already added the player, and such a player must still be removed before a map load.
    if (players->Num() != seat_ + 1)
    {
        platform::Log(L"join failed: the engine did not add a local player ({}; count now {})",
                      created.called ? L"the call returned" : L"the call raised an exception", players->Num());
        return added;
    }
    player_       = (*players)[seat_];  // the engine appends the new player
    gameInstance_ = gameInstance;
    ++joins_;

    unreal::UObject** field = unreal::ControllerField(player_);
    added.controller        = field == nullptr ? nullptr : *field;
    if (created.called && created.controller != added.controller)
    {
        platform::Log(L"WARNING: CreatePlayer returned a controller other than the one player {} lists", seat_ + 1);
    }
    return added;
}

bool LocalPlayers::Remove() noexcept
{
    if (!HasPlayer() || !StillListed())
    {
        return true;
    }
    auto* players = unreal::LocalPlayers(gameInstance_);

    // RemoveLocalPlayer reports success even for a player it did not hold, so membership before
    // (checked above) and after (below) is the evidence.
    if (!RemoveLocalPlayer(gameInstance_, player_, removeLocalPlayer_) || Contains(*players, player_))
    {
        platform::Log(L"leave failed: the engine did not remove player {}", seat_ + 1);
        return false;
    }
    Forget();
    return true;
}

bool LocalPlayers::StillListed() noexcept
{
    // Reads only the engine's list, never the player's object: if the engine removed the player on
    // its own, the object may already have been freed.
    auto* players = unreal::LocalPlayers(gameInstance_);
    if (players != nullptr && Contains(*players, player_))
    {
        return true;
    }
    Forget();
    return false;
}

void LocalPlayers::Forget() noexcept
{
    // The engine's garbage collector frees the player; this mod must not touch it again.
    player_       = nullptr;
    gameInstance_ = nullptr;
}

}  // namespace lss::players
