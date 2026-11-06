#include "players/local_players.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/seats.h"

#include <cstddef>

#include <Unreal/UFunction.hpp>

namespace lss::players {

std::int32_t LocalPlayerControllerId(const unreal::UObject* localPlayer) noexcept
{
    std::int32_t id = -1;
    if (localPlayer == nullptr ||
        !platform::CopyGuarded(&id, reinterpret_cast<const std::byte*>(localPlayer) + facts::kLocalPlayerControllerId,
                               sizeof(id)))
    {
        return -1;
    }
    return id;
}

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

/// The controller id of a listed local player that is neither player 1 (0) nor a built seat nor a mirror view nor
/// `joining`, or -1.
std::int32_t UnseatedId(RC::Unreal::TArray<unreal::UObject*>& players, std::int32_t joining) noexcept
{
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        const std::int32_t id = LocalPlayerControllerId(players[i]);
        if (id > 0 && id != joining && !SeatBuilt(id) && !IsMirrorId(id))  // an unread id (-1) is no evidence
        {
            return id;
        }
    }
    return -1;
}

/// The listed local players that are not mirror views (player 1, the seats, any other).
std::int32_t PlayersListed(RC::Unreal::TArray<unreal::UObject*>& players) noexcept
{
    std::int32_t n = 0;
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        n += IsMirrorId(LocalPlayerControllerId(players[i])) ? 0 : 1;
    }
    return n;
}

// UGameViewportClient's MaxSplitscreenPlayers in stock UE 4.26: CreateLocalPlayer refuses a fifth.
constexpr std::int32_t kEngineMaxLocalPlayers = 4;

/// A listed local player already has controller id `seat`.
bool SeatListed(RC::Unreal::TArray<unreal::UObject*>& players, std::int32_t seat) noexcept
{
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        if (LocalPlayerControllerId(players[i]) == seat)
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
/// a seat's player unless session/own_controller.h is installed and a join scope is open.
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
    // The room counts the players, not the mirror views: a seat's join takes the mirrors out right before its
    // CreatePlayer (views/mirror_player.h), and a mirror's add is limited to the columns by its module.
    const std::int32_t listed = players == nullptr ? -1 : PlayersListed(*players);
    if (players == nullptr || listed < 1 || listed >= kMaxPlayers || players->Num() >= kEngineMaxLocalPlayers)
    {
        if (log)
        {
            platform::Log(L"join refused: expected 1 to {} player(s) before the join, found {} ({} local player(s) "
                          L"listed with the mirror views)",
                          kMaxPlayers - 1, listed, players == nullptr ? -1 : players->Num());
        }
        return {};
    }
    // The seat is free when no listed player has its controller id (a list position names no seat: a leave closes
    // the list up). A listed player with no seat's id and no mirror's (an unbuilt seat's, left behind by a removal
    // that failed) refuses the join too: the new player would take a column the mod gives another.
    if (SeatListed(*players, seat_))
    {
        if (log)
        {
            platform::Log(L"join refused: a local player with controller id {} is listed already", seat_);
        }
        return {};
    }
    if (const std::int32_t stranger = UnseatedId(*players, seat_); stranger >= 0)
    {
        if (log)
        {
            platform::Log(L"join refused: a local player with controller id {} is listed, not player 2's or 3's", stranger);
        }
        return {};
    }
    unreal::UObject*  localPlayer = (*players)[0];  // player 1 is the list's first: no leave removes it
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
        platform::Log(L"{} is already in", Name());
        return added;
    }
    unreal::UObject* gameInstance = unreal::GameInstance();
    auto*            players      = unreal::LocalPlayers(gameInstance);
    if (players == nullptr)
    {
        return added;
    }

    const std::int32_t before  = players->Num();
    const auto         created = CreatePlayerWithController(playerOneController, seat_);
    added.raised               = !created.called;

    // The engine's list decides what happened, not the call: an exception can be raised after the
    // engine already added the player, and such a player must still be removed before a map load.
    if (players->Num() != before + 1)
    {
        platform::Log(L"join failed: the engine did not add a local player ({}; count now {})",
                      created.called ? L"the call returned" : L"the call raised an exception", players->Num());
        return added;
    }
    player_ = (*players)[before];  // the engine appends the new player, after whoever is listed
    if (const std::int32_t id = LocalPlayerControllerId(player_); id != seat_)
    {
        platform::Log(L"WARNING: the local player the engine appended has controller id {}, not {}", id, seat_);
    }
    gameInstance_ = gameInstance;
    ++joins_;

    unreal::UObject** field = unreal::ControllerField(player_);
    added.controller        = field == nullptr ? nullptr : *field;
    if (created.called && created.controller != added.controller)
    {
        platform::Log(L"WARNING: CreatePlayer returned a controller other than the one {} lists", Name());
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
        platform::Log(L"leave failed: the engine did not remove {}", Name());
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

const wchar_t* LocalPlayers::Name() const noexcept
{
    return IsMirrorId(seat_) ? L"a mirror view" : SeatName(seat_);
}

void LocalPlayers::Forget() noexcept
{
    // The engine's garbage collector frees the player; this mod must not touch it again.
    player_       = nullptr;
    gameInstance_ = nullptr;
}

}  // namespace lss::players
