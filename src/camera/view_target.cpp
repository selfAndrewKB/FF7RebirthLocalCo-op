#include "camera/view_target.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"

#include <cstdint>

namespace lss::camera::view_target {

namespace {

using SetViewTargetFn = void (*)(void* cameraManager, void* newTarget, const facts::ViewTargetTransition* params);

// Set once at install.
SetViewTargetFn g_setViewTarget = nullptr;

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

using camera_access::CameraManagerOf;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto setViewTarget = platform::FindFunction(image, facts::kSetViewTarget);
    if (!setViewTarget || !camera_access::Install(image))
    {
        return false;
    }
    g_setViewTarget       = reinterpret_cast<SetViewTargetFn>(*setViewTarget);
    return true;
}

bool Installed() noexcept
{
    return g_setViewTarget != nullptr;
}

void* Current(void* controller) noexcept
{
    return ReadPointer(CameraManagerOf(controller), facts::kCameraManagerViewTarget);
}

bool OnOwnCamera(void* controller) noexcept
{
    void* cameraManager = CameraManagerOf(controller);
    void* cameraActor   = ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor);
    return cameraActor != nullptr && ReadPointer(cameraManager, facts::kCameraManagerViewTarget) == cameraActor;
}

bool Aim(void* controller) noexcept
{
    void* cameraManager = CameraManagerOf(controller);
    void* cameraActor   = ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor);
    if (!Installed() || g_stopped || cameraActor == nullptr || !players::generation::OnGameThread())
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
    void* cameraManager = CameraManagerOf(controller);
    if (!Installed() || g_stopped || cameraManager == nullptr || actor == nullptr || !players::generation::OnGameThread())
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

}  // namespace lss::camera::view_target
