#include "player_two/charapop_facts.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/guarded.h"

namespace lss::player_two::charapop {

namespace {

using RC::Unreal::UObject;

// USceneComponent's world location (as handover_watch reads it).
constexpr std::uint32_t kComponentWorldLocation = 0x1D0;

std::int32_t g_rootOffset = -1;  // AActor::RootComponent, found once by reflection; game thread

}  // namespace

bool ActorLocation(void* actor, float (&out)[3]) noexcept
{
    if (actor == nullptr)
    {
        return false;
    }
    if (g_rootOffset < 0)
    {
        try
        {
            auto* property = static_cast<UObject*>(actor)->GetPropertyByNameInChain(STR("RootComponent"));
            if (property == nullptr || property->GetSize() != sizeof(void*))
            {
                return false;
            }
            g_rootOffset = property->GetOffset_Internal();
        }
        catch (...)
        {
            return false;
        }
    }
    const auto* root = static_cast<const std::byte*>(ReadPointer(actor, static_cast<std::uint32_t>(g_rootOffset)));
    return root != nullptr && platform::CopyGuarded(out, root + kComponentWorldLocation, sizeof(out));
}

std::wstring NameOf(void* actor)
{
    try
    {
        return actor != nullptr ? std::wstring(static_cast<UObject*>(actor)->GetName()) : std::wstring(L"?");
    }
    catch (...)
    {
        return L"?";
    }
}

}  // namespace lss::player_two::charapop
