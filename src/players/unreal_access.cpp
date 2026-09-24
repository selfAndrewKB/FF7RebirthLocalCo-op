#include "players/unreal_access.h"

#include "platform/engine_addresses.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::unreal {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

namespace {

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
// MSVC allows __try only in functions with no objects that need unwinding, hence these small
// separate functions.

int GuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

bool GuardedProcessEvent(UObject* target, UFunction* function, void* params) noexcept
{
    __try
    {
        target->ProcessEvent(function, params);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool GuardedRemove(RemoveLocalPlayerFn remove, UObject* gameInstance, UObject* localPlayer) noexcept
{
    __try
    {
        remove(gameInstance, localPlayer);  // its bool result is always true (RE doc §Q1)
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

UObject* GameInstance() noexcept
{
    return UObjectGlobals::FindFirstOf(STR("GameInstance"));  // subclasses match too
}

RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept
{
    if (gameInstance == nullptr)
    {
        return nullptr;
    }
    return gameInstance->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("LocalPlayers"));
}

UObject** ControllerField(UObject* localPlayer) noexcept
{
    if (localPlayer == nullptr)
    {
        return nullptr;
    }
    auto* field = localPlayer->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerController"));
    const auto offset = reinterpret_cast<std::uintptr_t>(field) - reinterpret_cast<std::uintptr_t>(localPlayer);
    return field != nullptr && offset == engine::kLocalPlayerControllerOffset ? field : nullptr;
}

bool CreatePlayerWithoutController(UObject* worldContext, std::int32_t controllerId) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.GameplayStatics:CreatePlayer"));
    auto* statics = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    if (function == nullptr || statics == nullptr || worldContext == nullptr ||
        function->GetParmsSize() != sizeof(CreatePlayerParams))
    {
        return false;
    }
    UObject* defaultObject = statics->GetClassDefaultObject();
    if (defaultObject == nullptr)
    {
        return false;
    }

    CreatePlayerParams params{};
    params.WorldContextObject     = worldContext;
    params.ControllerId           = controllerId;
    params.bSpawnPlayerController = false;
    return GuardedProcessEvent(defaultObject, function, &params);
}

bool RemoveLocalPlayer(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer) noexcept
{
    if (gameInstance == nullptr || localPlayer == nullptr)
    {
        return false;
    }
    // The game instance's class decides which vtable is used; RE found no class overriding this
    // entry, and this check makes that a precondition rather than an assumption.
    const auto vtable = *reinterpret_cast<const std::uintptr_t* const*>(gameInstance);
    const auto entry  = vtable[engine::kRemoveLocalPlayerVTableOffset / sizeof(std::uintptr_t)];
    if (entry != removeLocalPlayer)
    {
        return false;
    }
    return GuardedRemove(reinterpret_cast<RemoveLocalPlayerFn>(entry), gameInstance, localPlayer);
}

}  // namespace lss::players::unreal
