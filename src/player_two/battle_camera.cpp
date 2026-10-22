#include "player_two/battle_camera.h"

#include "dev/dev_switch.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/camera_access.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::battle_camera {

namespace {

// Facts read from the database (RE-p2-battle-camera.md; this build).
constexpr std::uint32_t  kOperatorState        = 0xD0;    // u8 (the Field operator's active byte is at the same place)
constexpr std::uint32_t  kOperatorActive       = 0xD1;    // u8: the update picks the lowest-numbered active operator
constexpr std::uint32_t  kOperatorBytes        = 0x2CD0;  // current / previous operator bytes (+0x2CD0..+0x2CD3)

constexpr std::uint8_t kActivateBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8,
                                             0x7C, 0xAD, 0xEA, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x01, 0x41, 0x8A};
constexpr std::uint8_t kDeactivateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                             0xA8, 0x5F, 0x00, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9, 0x41, 0x8A};
constexpr platform::SlotFact kActivate{L"the Battle operator's Activate slot (+0x10)", 0x1466B4938,
                                       {L"FEndBattleCameraOperator::Activate", 0x1412041A4, kActivateBytes}};
constexpr platform::SlotFact kDeactivate{L"the Battle operator's Deactivate slot (+0x20)", 0x1466B4948,
                                         {L"FEndBattleCameraOperator::Deactivate", 0x1410A8F78, kDeactivateBytes}};

// The operator's camera modules (EEndCameraModuleLayer): +0x6E0 points at 25 TSharedPtr entries (16
// bytes each), each module's active bytes at +0x44/+0x45. Logged: battle offset, length, lock-on,
// look-at, skill, command, leader change, scan.
constexpr std::uint32_t kModuleActive = 0x44;
constexpr std::uint32_t kLoggedModules[] = {5, 6, 7, 16, 20, 21, 22, 23};


using StateFn = void (*)(void* battleOperator, float blendTime, bool flag);

// Set once at install.
platform::VTableEntry g_activateSlot;
platform::VTableEntry g_deactivateSlot;
StateFn               g_engineActivate      = nullptr;
StateFn               g_engineDeactivate    = nullptr;

// Game thread.

/// Player 1's and player 2's Battle operators, while player 2 holds a party member (player_two/camera_access.h).
using Pair = camera_access::OperatorPair;
using camera_access::CameraManagerOf;
using camera_access::BattleOperatorOf;

std::optional<Pair> CurrentPair() noexcept
{
    return camera_access::CurrentPair(&BattleOperatorOf);
}

std::uint8_t Active(const void* battle) noexcept
{
    return battle == nullptr ? 0 : ReadField<std::uint8_t>(battle, kOperatorActive);
}

bool CallGuarded(StateFn engine, void* battle, float blendTime, bool flag) noexcept
{
    __try
    {
        engine(battle, blendTime, flag);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The engine's call on player 2's operator, with player 2 as the primary controller.
void OnPlayerTwo(StateFn engine, void* battle, float blendTime, bool flag)
{
    const primary_controller::Scope scope;
    const bool                      ok = CallGuarded(engine, battle, blendTime, flag);
    platform::Log(L"battle camera: player 2's Battle operator {} (primary scope {}); active now {}{}",
                  engine == g_engineActivate ? L"activated" : L"deactivated", scope.Held() ? L"held" : L"REFUSED",
                  Active(battle), ok ? L"" : L"; the engine call RAISED an exception");
}

// The two replacements: the engine's call first, then the same on player 2's Battle operator when
// `battle` is player 1's.

void Activate(void* battle, float blendTime, bool flag)
{
    g_engineActivate(battle, blendTime, flag);
    const auto pair = CurrentPair();
    if (pair && battle == pair->one)
    {
        OnPlayerTwo(g_engineActivate, pair->two, blendTime, flag);
    }
}

void Deactivate(void* battle, float blendTime, bool flag)
{
    g_engineDeactivate(battle, blendTime, flag);
    const auto pair = CurrentPair();
    if (pair && battle == pair->one)
    {
        OnPlayerTwo(g_engineDeactivate, pair->two, blendTime, flag);
    }
}

#if LSS_DEV
/// What the watch (dev tools) logs of one camera manager, as raw values: compared each frame, formatted only
/// when it changes.
struct CameraState
{
    bool         present = false;
    std::uint8_t operators[4]{};  // current / previous operator bytes
    std::uint8_t battle[2]{};     // the Battle operator's state and active bytes
    std::uint8_t field[2]{};      // the Field operator's
    std::uint8_t modules[std::size(kLoggedModules)][2]{};
    bool         moduleRead[std::size(kLoggedModules)]{};
    bool operator==(const CameraState&) const = default;
};

CameraState StateOf(void* cameraManager) noexcept
{
    // Every read guarded: these are the game's heap objects, and a log must never be able to crash it.
    CameraState state;
    if (cameraManager == nullptr)
    {
        return state;
    }
    state.present = true;
    void* battle  = BattleOperatorOf(cameraManager);
    void* field   = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    Read(reinterpret_cast<std::uintptr_t>(cameraManager) + kOperatorBytes, state.operators);
    const auto battleAt = reinterpret_cast<std::uintptr_t>(battle);
    const auto fieldAt  = reinterpret_cast<std::uintptr_t>(field);
    Read(battleAt != 0 ? battleAt + kOperatorState : 0, state.battle[0]);
    Read(battleAt != 0 ? battleAt + kOperatorActive : 0, state.battle[1]);
    Read(fieldAt != 0 ? fieldAt + kOperatorState : 0, state.field[0]);
    Read(fieldAt != 0 ? fieldAt + kOperatorActive : 0, state.field[1]);
    for (std::size_t i = 0; i < std::size(kLoggedModules); ++i)
    {
        const auto module   = reinterpret_cast<std::uintptr_t>(camera_access::ModuleAt(battle, kLoggedModules[i]));
        state.moduleRead[i] = Read(module != 0 ? module + kModuleActive : 0, state.modules[i]);
    }
    return state;
}

std::wstring Describe(const CameraState& state)
{
    if (!state.present)
    {
        return L"-";
    }
    std::wstring modules;
    for (std::size_t i = 0; i < std::size(kLoggedModules); ++i)
    {
        modules += state.moduleRead[i] ? std::format(L" {}:{}{}", kLoggedModules[i], state.modules[i][0], state.modules[i][1])
                                       : std::format(L" {}:-", kLoggedModules[i]);
    }
    return std::format(L"operators cur {} prev {} ({} {}) battle {}/{} field {}/{} modules{}", state.operators[0],
                       state.operators[1], state.operators[2], state.operators[3], state.battle[0], state.battle[1],
                       state.field[0], state.field[1], modules);
}

std::array<CameraState, 2> g_logged{};  // game thread: both camera managers as last logged
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto activate   = platform::FindFunction(image, kActivate.target);
    const auto deactivate = platform::FindFunction(image, kDeactivate.target);
    if (!activate || !deactivate || !camera_access::Install(image) || !primary_controller::Installed())
    {
        platform::Log(L"battle camera: the Battle operator is not what the research read; player 2's camera stays on "
                      L"the Field operator in battle");
        return false;
    }
    g_engineActivate      = reinterpret_cast<StateFn>(*activate);
    g_engineDeactivate    = reinterpret_cast<StateFn>(*deactivate);
    const bool installed =
        platform::ReplaceSlot(image, kActivate, reinterpret_cast<std::uintptr_t>(&Activate), g_activateSlot) &&
        platform::ReplaceSlot(image, kDeactivate, reinterpret_cast<std::uintptr_t>(&Deactivate), g_deactivateSlot);
    if (!installed)
    {
        g_activateSlot.Restore();  // both or neither: an activation with no deactivation would never end
        g_deactivateSlot.Restore();
    }
    platform::Log(L"battle camera: {}", installed ? L"player 2's Battle operator follows player 1's switches"
                                                  : L"the slots could not be replaced; not installed");
    return installed;
}

void Remove() noexcept
{
    for (auto* slot : {&g_deactivateSlot, &g_activateSlot})
    {
        if (!slot->Restore())
        {
            platform::Log(L"could not restore a Battle-operator slot; the replacement stays in place");
        }
    }
}

bool Seed() noexcept
{
    if (g_engineActivate == nullptr)
    {
        return false;
    }
    const auto pair = CurrentPair();
    if (!pair)
    {
        return false;
    }
    if (Active(pair->one) != Active(pair->two))
    {
        OnPlayerTwo(Active(pair->one) != 0 ? g_engineActivate : g_engineDeactivate, pair->two, 0.0f, false);
    }
    return Active(pair->one) == Active(pair->two);
}

bool PlayerTwoBattleActive(void* cameraManager) noexcept
{
    return camera_access::Installed() && Active(BattleOperatorOf(cameraManager)) != 0;
}

void Watch() noexcept
{
#if LSS_DEV
    if (!platform::Enabled(platform::Level::Debug) || !generation::OnGameThread() ||
        !generation::Active())
    {
        return;
    }
    const std::array<CameraState, 2> now{StateOf(CameraManagerOf(generation::PlayerOneController())),
                                         StateOf(CameraManagerOf(generation::PlayerTwoController()))};
    if (now == g_logged)
    {
        return;
    }
    g_logged = now;
    try
    {
        platform::LogDebug(L"battle camera: P1 {} | P2 {}", Describe(now[0]), Describe(now[1]));
    }
    catch (...)
    {
    }
#endif
}

}  // namespace lss::player_two::battle_camera
