#include "player_two/camera/camera_access.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/image.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

namespace lss::player_two::camera_access {

namespace {

constexpr std::uint32_t kModuleEntry = 16;  // a TSharedPtr slot: the module pointer first

std::uintptr_t g_cameraManagerVTable = 0;
std::uintptr_t g_fieldVTable         = 0;
std::uintptr_t g_battleVTable        = 0;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (Installed())
    {
        return true;
    }
    const auto camera = image.Rebase(facts::kCameraManagerVTable);
    const auto field  = image.Rebase(facts::kFieldOperatorVTable);
    const auto battle = image.Rebase(facts::kBattleOperatorVTable);
    if (!camera || !field || !battle)
    {
        return false;
    }
    g_cameraManagerVTable = *camera;
    g_fieldVTable         = *field;
    g_battleVTable        = *battle;
    return true;
}

bool Installed() noexcept
{
    return g_cameraManagerVTable != 0 && g_fieldVTable != 0 && g_battleVTable != 0;
}

void* CameraManagerOf(const void* controller) noexcept
{
    void* cameraManager = ReadPointer(controller, facts::kControllerCameraManager);
    if (g_cameraManagerVTable == 0 || VTableOf(cameraManager) != g_cameraManagerVTable ||
        ReadPointer(cameraManager, facts::kCameraManagerPcOwner) != controller)
    {
        return nullptr;
    }
    return cameraManager;
}

void* FieldOperatorOf(const void* cameraManager) noexcept
{
    void* field = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    return g_fieldVTable != 0 && VTableOf(field) == g_fieldVTable ? field : nullptr;
}

void* BattleOperatorOf(const void* cameraManager) noexcept
{
    void* battle = ReadPointer(cameraManager, facts::kCameraManagerBattleOperator);
    return g_battleVTable != 0 && VTableOf(battle) == g_battleVTable ? battle : nullptr;
}

const void* ModuleAt(const void* op, std::uint32_t slot) noexcept
{
    const void* modules = ReadPointer(op, facts::kOperatorModules);
    return modules != nullptr ? ReadPointer(modules, slot * kModuleEntry) : nullptr;
}

int PlayerOfOperator(const void* op) noexcept
{
    if (op == nullptr)
    {
        return -1;
    }
    // The controller's camera manager as it stands (no vtable check: a caller may ask before any install).
    const void* const controllers[2] = {generation::PlayerOneController(),
                                        generation::Active() ? generation::PlayerTwoController() : nullptr};
    for (int p = 0; p < 2; ++p)
    {
        const void* pcm = ReadPointer(controllers[p], facts::kControllerCameraManager);
        if (pcm != nullptr && (ReadPointer(pcm, facts::kCameraManagerFieldOperator) == op ||
                               ReadPointer(pcm, facts::kCameraManagerBattleOperator) == op))
        {
            return p;
        }
    }
    return -1;
}

std::optional<OperatorPair> CurrentPair(OperatorOf operatorOf) noexcept
{
    if (!generation::OnGameThread() || !generation::Active() || generation::Member() == nullptr)
    {
        return std::nullopt;
    }
    void* one = operatorOf(CameraManagerOf(generation::PlayerOneController()));
    void* two = operatorOf(CameraManagerOf(generation::PlayerTwoController()));
    if (one == nullptr || two == nullptr || one == two)
    {
        return std::nullopt;
    }
    return OperatorPair{one, two};
}

}  // namespace lss::player_two::camera_access
