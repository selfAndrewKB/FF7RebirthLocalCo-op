#include "player_two/camera_distance.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::camera_distance {

namespace {

// Facts read from the database (RE-p2-battle-camera.md §6; this build).
constexpr std::uint8_t kPrimaryApplyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                               0x34, 0x5A, 0xCB, 0xFE, 0x48, 0x2B, 0xE0, 0x8A, 0xDA, 0x40, 0x8A, 0xF9};
constexpr platform::FunctionFact kPrimaryApply{L"PrimaryPCM_ApplyCameraDistanceOptions", 0x1433F94EC, kPrimaryApplyBytes};
constexpr std::uint8_t kManagerApplyBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xE2, 0x5B, 0x37, 0xFE, 0x48, 0x2B,
                                               0xE0, 0x4C, 0x8B, 0xD1, 0x45, 0x8A, 0xD8, 0x48, 0x8B, 0x89, 0xB0, 0x2C};
constexpr platform::FunctionFact kManagerApply{L"AEndPlayerCameraManager__ApplyCameraDistanceOptions", 0x143D39344,
                                               kManagerApplyBytes};
constexpr std::uintptr_t kSetterCall  = 0x142ADCBCA;  // in the option setter: the primary camera manager's apply
constexpr std::uintptr_t kConfirmCall = 0x14180A4D6;  // in the confirm-key handler: the per-camera-manager apply
constexpr int            kMaxLines    = 50;

using PrimaryApplyFn = void (*)(std::uint8_t field, std::uint8_t battle);
using ManagerApplyFn = void (*)(void* cameraManager, std::uint8_t field, std::uint8_t battle);

// Set once at install.
platform::CallRedirect g_setterCall;
platform::CallRedirect g_confirmCall;
ManagerApplyFn         g_managerApply        = nullptr;
std::uintptr_t         g_cameraManagerVTable = 0;

// Game thread.
int g_lines = 0;

void Line(const std::wstring& text) noexcept
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"camera distance: {}", text);
    }
}

/// A controller's End camera manager, if it is one and names the controller as its owner.
void* CameraManagerOf(void* controller) noexcept
{
    void* cameraManager = ReadPointer(controller, facts::kControllerCameraManager);
    if (VTableOf(cameraManager) != g_cameraManagerVTable ||
        ReadPointer(cameraManager, facts::kCameraManagerPcOwner) != controller)
    {
        return nullptr;
    }
    return cameraManager;
}

/// The game's per-camera-manager apply for player 2's camera manager, after the game's own apply on
/// `applied` (the primary camera manager).
void ApplyToPlayerTwo(const void* applied, std::uint8_t field, std::uint8_t battle) noexcept
{
    if (!generation::OnGameThread() || !generation::Active())
    {
        return;
    }
    void* two = CameraManagerOf(generation::PlayerTwoController());
    if (two == nullptr || two == applied)
    {
        return;
    }
    const bool ok = platform::CallGuarded(g_managerApply, two, field, battle);
    Line(std::format(L"field {} battle {} applied to player 2's camera manager too{}", field, battle,
                     ok ? L"" : L" (FAULTED)"));
}

/// Replaces the option setter's call of the primary camera manager's apply.
void PrimaryApply(std::uint8_t field, std::uint8_t battle) noexcept
{
    reinterpret_cast<PrimaryApplyFn>(g_setterCall.Original())(field, battle);
    ApplyToPlayerTwo(CameraManagerOf(generation::PlayerOneController()), field, battle);
}

/// Replaces the confirm-key handler's call of the per-camera-manager apply (on the primary one).
void ManagerApply(void* cameraManager, std::uint8_t field, std::uint8_t battle) noexcept
{
    reinterpret_cast<ManagerApplyFn>(g_confirmCall.Original())(cameraManager, field, battle);
    ApplyToPlayerTwo(cameraManager, field, battle);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto primary = platform::FindFunction(image, kPrimaryApply);
    const auto manager = platform::FindFunction(image, kManagerApply);
    const auto vtable  = image.Rebase(facts::kCameraManagerVTable);
    if (!primary || !manager || !vtable)
    {
        platform::Log(L"camera distance: UNAVAILABLE (a check failed); player 2's camera keeps the distance it started "
                      L"with");
        return false;
    }
    g_managerApply        = reinterpret_cast<ManagerApplyFn>(*manager);
    g_cameraManagerVTable = *vtable;
    const auto redirect = [&image](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && call.Install(*live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    const bool setter  = redirect(g_setterCall, kSetterCall, *primary, &PrimaryApply);
    const bool confirm = redirect(g_confirmCall, kConfirmCall, *manager, &ManagerApply);
    platform::Log(L"camera distance: the Options distance reaches player 2's camera from the option setter {} and the "
                  L"confirm key {}",
                  setter ? L"yes" : L"NO (check failed)", confirm ? L"yes" : L"NO (check failed)");
    return setter || confirm;
}

void Remove() noexcept
{
    for (auto* call : {&g_setterCall, &g_confirmCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"camera distance: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::camera_distance
