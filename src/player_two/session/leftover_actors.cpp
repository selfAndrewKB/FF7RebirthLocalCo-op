#include "player_two/session/leftover_actors.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <atomic>
#include <cstdint>
#include <initializer_list>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::leftover_actors {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using players::unreal::GuardedProcessEvent;

constexpr std::uint32_t kItemFlags  = 0x08;        // FUObjectItem: internal flags (both builds)
constexpr std::int32_t  kPendingKill = 0x20000000;  // EInternalObjectFlags
constexpr std::int32_t  kRootSet     = 0x40000000;

/// `object` if it is a `classPath`, else null.
void* AsA(void* object, const wchar_t* classPath) noexcept
{
    auto* cls       = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
    auto* candidate = static_cast<UObject*>(object);
    return candidate != nullptr && cls != nullptr && candidate->IsA(cls) ? object : nullptr;
}

/// `object` if it is an actor, else null.
void* AsActor(void* object) noexcept
{
    return AsA(object, STR("/Script/Engine.Actor"));
}

/// UObject::MarkPendingKill as the engine inlines it (stock UE 4.26): an atomic OR of PendingKill into
/// the object's slot flags, refused for a root-set object (the engine checks the same). False when the
/// slot no longer holds `object` or it is rooted.
bool MarkPendingKill(void* object) noexcept
{
    auto* item = RC::Unreal::UObjectArray::IndexToObject(static_cast<UObject*>(object)->GetInternalIndex());
    if (item == nullptr || item->GetUObject() != object)
    {
        return false;
    }
    std::atomic_ref<std::int32_t> flags(*reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(item) + kItemFlags));
    // One compare-and-swap, as the engine's ThisThreadAtomicallySetFlag: the root-set test holds for the word written.
    std::int32_t now = flags.load(std::memory_order_relaxed);
    do
    {
        if ((now & kRootSet) != 0)
        {
            return false;
        }
    } while (!flags.compare_exchange_weak(now, now | kPendingKill));
    return true;
}

}  // namespace

Actors Collect(void* playerTwoController) noexcept
{
    Actors actors;
    if (playerTwoController == nullptr)
    {
        return actors;
    }
    void* cameraManager = ReadPointer(playerTwoController, facts::kControllerCameraManager);
    actors.trigger      = AsActor(ReadPointer(playerTwoController, facts::kControllerTrigger));
    actors.battleTalk   = AsActor(ReadPointer(playerTwoController, facts::kControllerBattleTalkActor));
    actors.cameraActor  = AsActor(ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor));
    actors.playerInput  = AsA(ReadPointer(playerTwoController, facts::kControllerPlayerInput), STR("/Script/Engine.PlayerInput"));

    auto* component = static_cast<UObject*>(ReadPointer(cameraManager, facts::kCameraManagerCollisionComponent));
    if (component != nullptr)
    {
        actors.collisionActor = AsActor(component->GetOuterPrivate());
    }
    return actors;
}

Result Destroy(const Actors& actors) noexcept
{
    Result result;
    result.inputReleased = actors.playerInput != nullptr && MarkPendingKill(actors.playerInput);
    auto*  destroy =
        UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_DestroyActor"));
    if (destroy == nullptr || destroy->GetParmsSize() != 0)
    {
        platform::Log(L"AActor::K2_DestroyActor not found as expected; player 2's leftover actors stay");
        return result;
    }
    for (void* actor : {actors.trigger, actors.battleTalk, actors.cameraActor, actors.collisionActor})
    {
        if (actor == nullptr)
        {
            continue;
        }
        if (GuardedProcessEvent(static_cast<UObject*>(actor), destroy, nullptr))
        {
            ++result.destroyed;
        }
        else
        {
            ++result.failed;
        }
    }
    return result;
}

}  // namespace lss::player_two::leftover_actors
