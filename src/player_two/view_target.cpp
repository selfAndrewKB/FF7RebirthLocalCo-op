#include "player_two/view_target.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <cstdint>

namespace lss::player_two::view_target {

namespace {

using SetViewTargetFn = void (*)(void* cameraManager, void* newTarget, const facts::ViewTargetTransition* params);

// Set once at install.
SetViewTargetFn g_setViewTarget      = nullptr;
std::uintptr_t  g_cameraManagerVTable = 0;

// Game thread only: set once the engine raised an exception inside SetViewTarget.
bool g_stopped = false;

/// The engine's SetViewTarget inside a structured-exception guard (platform/guarded.h).
bool GuardedSetViewTarget(void* cameraManager, void* cameraActor) noexcept
{
    static constexpr facts::ViewTargetTransition kParams;
    __try
    {
        g_setViewTarget(cameraManager, cameraActor, &kParams);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The controller's camera manager, if it is an End one whose owner is the controller.
void* OwnedCameraManager(void* controller) noexcept
{
    void* cameraManager = ReadPointer(controller, facts::kControllerCameraManager);
    if (g_cameraManagerVTable == 0 || VTableOf(cameraManager) != g_cameraManagerVTable ||
        ReadPointer(cameraManager, facts::kCameraManagerPcOwner) != controller)
    {
        return nullptr;
    }
    return cameraManager;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto setViewTarget = platform::FindFunction(image, facts::kSetViewTarget);
    const auto vtable        = image.Rebase(facts::kCameraManagerVTable);
    if (!setViewTarget || !vtable)
    {
        return false;
    }
    g_setViewTarget       = reinterpret_cast<SetViewTargetFn>(*setViewTarget);
    g_cameraManagerVTable = *vtable;
    return true;
}

bool Installed() noexcept
{
    return g_setViewTarget != nullptr;
}

void* Current(void* controller) noexcept
{
    return ReadPointer(OwnedCameraManager(controller), facts::kCameraManagerViewTarget);
}

bool OnOwnCamera(void* controller) noexcept
{
    void* cameraManager = OwnedCameraManager(controller);
    void* cameraActor   = ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor);
    return cameraActor != nullptr && ReadPointer(cameraManager, facts::kCameraManagerViewTarget) == cameraActor;
}

bool Aim(void* controller) noexcept
{
    void* cameraManager = OwnedCameraManager(controller);
    void* cameraActor   = ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor);
    if (!Installed() || g_stopped || cameraActor == nullptr || !generation::OnGameThread())
    {
        return false;
    }
    if (!GuardedSetViewTarget(cameraManager, cameraActor))
    {
        g_stopped = true;
        platform::Log(L"own camera view STOPPED: the engine raised an exception inside SetViewTarget; player 2's "
                      L"camera manager is no longer aimed this session");
        return false;
    }
    return OnOwnCamera(controller);
}

bool AimAt(void* controller, void* actor) noexcept
{
    void* cameraManager = OwnedCameraManager(controller);
    if (!Installed() || g_stopped || cameraManager == nullptr || actor == nullptr || !generation::OnGameThread())
    {
        return false;
    }
    if (!GuardedSetViewTarget(cameraManager, actor))
    {
        g_stopped = true;
        platform::Log(L"camera view STOPPED: the engine raised an exception inside SetViewTarget (aiming a camera manager "
                      L"at another actor); no camera manager is aimed again this session");
        return false;
    }
    return ReadPointer(cameraManager, facts::kCameraManagerViewTarget) == actor;
}

}  // namespace lss::player_two::view_target
