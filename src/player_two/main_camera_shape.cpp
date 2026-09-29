#include "player_two/main_camera_shape.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/image.h"
#include "platform/log.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "platform/guarded.h"
#include "players/tracked_object.h"
#include "players/unreal_access.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::player_two::main_camera_shape {

namespace {

using RC::Unreal::FBoolProperty;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

// RE-p2-hud-view-levers.md §3 (this build): the component the ACameraActor constructor makes, and
// the bit its constructor sets and the Blueprint setter stores. Both checked by reflection.
constexpr std::int32_t kCameraComponentOffset    = 0x230;
constexpr std::int32_t kConstrainAspectRatioByte = 0x20C;

using Tracked = players::TrackedObject;


/// A main camera whose constraint the mod cleared.
struct Changed
{
    Tracked camera;
    Tracked component;
    bool    original = true;
};

/// The reflected pieces, found once UObject reflection is complete.
struct Reflection
{
    UClass*        cameraActor = nullptr;
    FBoolProperty* constrain   = nullptr;  // UCameraComponent.bConstrainAspectRatio
    UFunction*     setter      = nullptr;  // UCameraComponent.SetConstraintAspectRatio
    std::int32_t   setterValue = -1;       // its bool parameter's offset in the frame
};

constexpr std::int32_t kSetterFrameBytes = 16;
constexpr std::size_t  kMaxChanged       = 4;  // two players, and a respawned camera each

// Set once at install.
std::uintptr_t g_cameraManagerVTable = 0;

// Game thread.
Reflection                         g_reflection;
bool                               g_reflectionFound  = false;
bool                               g_reflectionFailed = false;
std::array<Changed, kMaxChanged>   g_changed{};
bool                               g_refusalLogged = false;

/// A pointer field read under a guard (the object holding it may have gone), or null.
void* ReadPointerGuarded(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return object != nullptr && platform::CopyGuarded(&value, static_cast<const std::byte*>(object) + offset,
                                                       sizeof(value))
               ? value
               : nullptr;
}

void* CameraManagerOf(void* controller) noexcept
{
    void* cameraManager = ReadPointerGuarded(controller, facts::kControllerCameraManager);
    if (cameraManager == nullptr ||
        reinterpret_cast<std::uintptr_t>(ReadPointerGuarded(cameraManager, 0)) != g_cameraManagerVTable ||
        ReadPointerGuarded(cameraManager, facts::kCameraManagerPcOwner) != controller)
    {
        return nullptr;
    }
    return cameraManager;
}

bool FindReflection() noexcept
{
    if (g_reflectionFound || g_reflectionFailed)
    {
        return g_reflectionFound;
    }
    try
    {
        Reflection found;
        found.cameraActor = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CameraActor"));
        auto* componentClass =
            UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CameraComponent"));
        found.setter = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.CameraComponent:SetConstraintAspectRatio"));
        UObject* component = componentClass != nullptr ? static_cast<UObject*>(componentClass->GetClassDefaultObject()) : nullptr;
        auto* property  = component != nullptr ? component->GetPropertyByNameInChain(STR("bConstrainAspectRatio")) : nullptr;
        found.constrain = property != nullptr ? RC::Unreal::CastField<FBoolProperty>(property) : nullptr;
        auto* parameter = found.setter != nullptr ? found.setter->FindProperty(RC::Unreal::FName(
                                                        STR("bInConstrainAspectRatio"), RC::Unreal::FNAME_Find))
                                                  : nullptr;
        if (parameter != nullptr && parameter->GetSize() == 1 && parameter->GetOffset_Internal() >= 0 &&
            parameter->GetOffset_Internal() < found.setter->GetParmsSize() &&
            found.setter->GetParmsSize() <= kSetterFrameBytes)
        {
            found.setterValue = parameter->GetOffset_Internal();
        }
        const bool holds = found.cameraActor != nullptr && found.constrain != nullptr && found.setterValue >= 0 &&
                           found.constrain->GetOffset_Internal() + found.constrain->GetByteOffset() ==
                               kConstrainAspectRatioByte;
        if (!holds)
        {
            // UE4SS may not have finished reflection yet; retried each tick until the split ends.
            return false;
        }
        g_reflection      = found;
        g_reflectionFound = true;
        platform::Log(L"main cameras: the stock SetConstraintAspectRatio setter and bConstrainAspectRatio (+0x{:X}) found",
                      kConstrainAspectRatioByte);
        return true;
    }
    catch (...)
    {
        g_reflectionFailed = true;
        platform::Log(L"main cameras: reflection failed (an exception); split halves keep the game's 16:9 bars");
        return false;
    }
}

bool IsTracked(const UObject* camera) noexcept
{
    for (const auto& changed : g_changed)
    {
        if (changed.camera.object == camera && changed.camera.Alive())
        {
            return true;
        }
    }
    return false;
}

/// The camera's component, if reflection places it where the research read it.
UObject* ComponentOf(UObject* camera) noexcept
{
    auto** field = camera->GetValuePtrByPropertyNameInChain<UObject*>(STR("CameraComponent"));
    const auto offset = reinterpret_cast<std::uintptr_t>(field) - reinterpret_cast<std::uintptr_t>(camera);
    UObject* component = field != nullptr && offset == kCameraComponentOffset ? *field : nullptr;
    return players::IsLiveObject(component) ? component : nullptr;
}

bool SetConstraint(UObject* component, bool value) noexcept
{
    alignas(16) std::uint8_t frame[kSetterFrameBytes]{};
    frame[g_reflection.setterValue] = value ? 1 : 0;
    return players::unreal::GuardedProcessEvent(component, g_reflection.setter, frame);
}

/// Clears the constraint of `controller`'s main camera once, remembering its value.
void ShapeMainCamera(void* controller, const wchar_t* who) noexcept
{
    void* cameraManager = CameraManagerOf(controller);
    auto* camera = static_cast<UObject*>(ReadPointerGuarded(cameraManager, facts::kCameraManagerEndCameraActor));
    if (camera == nullptr || IsTracked(camera) || !players::IsLiveObject(camera))
    {
        return;  // none, already set, or not a live object (reflection is not run on it)
    }
    Changed* slot = nullptr;
    for (auto& changed : g_changed)
    {
        slot = slot == nullptr && !changed.camera.Alive() ? &changed : slot;
    }
    UObject* component = camera->IsA(g_reflection.cameraActor) ? ComponentOf(camera) : nullptr;
    if (component == nullptr || slot == nullptr)
    {
        if (!g_refusalLogged)
        {
            g_refusalLogged = true;
            platform::Log(L"main cameras: {}'s main camera is not the ACameraActor the research read (or no room); its "
                          L"half keeps the game's 16:9 bars",
                          who);
        }
        return;
    }
    const bool original = g_reflection.constrain->GetPropertyValueInContainer(component);
    *slot = {Tracked::Of(camera), Tracked::Of(component), original};
    const bool set = !original || SetConstraint(component, false);
    platform::Log(L"main cameras: {}'s main camera {} (constrain aspect ratio was {})", who,
                  !original ? L"already unconstrained; left as it is"
                  : set     ? L"unconstrained with the game's SetConstraintAspectRatio(false): the half fills"
                            : L"setter RAISED AN EXCEPTION; the half keeps the game's 16:9 bars",
                  original);
}

void RestoreAll() noexcept
{
    int restored = 0;
    for (auto& changed : g_changed)
    {
        if (changed.camera.object == nullptr)
        {
            continue;
        }
        UObject* component = changed.component.object;
        if (changed.original && changed.component.Alive() &&
            !g_reflection.constrain->GetPropertyValueInContainer(component) && SetConstraint(component, true))
        {
            ++restored;
        }
        changed = {};
    }
    if (restored > 0)
    {
        platform::Log(L"main cameras: {} main camera(s) constrained to 16:9 again (the game's own setting)", restored);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto vtable = image.Rebase(facts::kCameraManagerVTable);
    g_cameraManagerVTable = vtable ? *vtable : 0;
    if (g_cameraManagerVTable == 0)
    {
        platform::Log(L"main cameras: the camera manager's vtable is not where the research read it; split halves keep "
                      L"the game's 16:9 bars");
    }
    return g_cameraManagerVTable != 0;
}

void Update(bool fill) noexcept
{
    try
    {
        if (!fill)
        {
            if (g_reflectionFound)
            {
                RestoreAll();
            }
            g_refusalLogged = false;
            return;
        }
        if (g_cameraManagerVTable == 0 || !FindReflection())
        {
            return;
        }
        ShapeMainCamera(generation::PlayerOneController(), L"player 1");
        ShapeMainCamera(generation::PlayerTwoController(), L"player 2");
    }
    catch (...)
    {
        platform::Log(L"main cameras: failed (an exception); left as they are");
    }
}

}  // namespace lss::player_two::main_camera_shape
