#include "camera/camera_access.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/image.h"
#include "players/actor_fields.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"

namespace lss::camera::camera_access {

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
    const auto camera = players::game_globals::Address(players::game_globals::Global::CameraManagerVTable);
    const auto field  = image.Rebase(facts::kFieldOperatorVTable);
    const auto battle = image.Rebase(facts::kBattleOperatorVTable);
    if (!camera || !field || !battle)
    {
        return false;
    }
    g_cameraManagerVTable = camera;
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
    void* cameraManager = players::actor_fields::CameraManager(controller);
    if (g_cameraManagerVTable == 0 || VTableOf(cameraManager) != g_cameraManagerVTable ||
        players::actor_fields::CameraOwner(cameraManager) != controller)
    {
        return nullptr;
    }
    return cameraManager;
}

void* FieldOperatorOf(const void* cameraManager) noexcept
{
    void* field = players::actor_fields::FieldOperator(cameraManager);
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

namespace {

/// Each seat's camera manager as its controller's field stands (no vtable check: a caller may ask before any
/// install): player 1's, then each built seat's while its generation is active; null elsewhere.
void CameraManagers(const void* (&managers)[players::kMaxPlayers]) noexcept
{
    for (int seat = 0; seat < players::kMaxPlayers; ++seat)
    {
        const bool counted = seat == 0 || (players::SeatBuilt(seat) && players::generation::Active(seat));
        managers[seat] = counted ? players::actor_fields::CameraManager(players::generation::ControllerOf(seat)) : nullptr;
    }
}

}  // namespace

int SeatOfCameraManager(const void* cameraManager) noexcept
{
    const void* managers[players::kMaxPlayers] = {};
    CameraManagers(managers);
    return SeatIn(managers, cameraManager);
}

int SeatOfOperator(const void* op) noexcept
{
    if (op == nullptr)
    {
        return -1;
    }
    const void* managers[players::kMaxPlayers] = {};
    CameraManagers(managers);
    for (int seat = 0; seat < players::kMaxPlayers; ++seat)
    {
        const void* pcm = managers[seat];
        if (pcm != nullptr && (players::actor_fields::FieldOperator(pcm) == op ||
                               ReadPointer(pcm, facts::kCameraManagerBattleOperator) == op))
        {
            return seat;
        }
    }
    return -1;
}

std::optional<OperatorPair> CurrentPair(int seat, OperatorOf operatorOf) noexcept
{
    if (!players::SeatBuilt(seat) || !players::generation::OnGameThread() || !players::generation::Active(seat) ||
        players::generation::CameraMemberOf(seat) == nullptr)
    {
        return std::nullopt;
    }
    void* operatorOne  = operatorOf(CameraManagerOf(players::generation::ControllerOf(0)));
    void* seatOperator = operatorOf(CameraManagerOf(players::generation::ControllerOf(seat)));
    if (operatorOne == nullptr || seatOperator == nullptr || operatorOne == seatOperator)
    {
        return std::nullopt;
    }
    return OperatorPair{operatorOne, seatOperator};
}

}  // namespace lss::camera::camera_access
