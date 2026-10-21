#include "player_two/field_camera.h"

#include "facts/cameras.h"
#include "facts/command_controller.h"
#include "facts/controller.h"
#include "platform/every.h"
#include "platform/image.h"
#include "player_two/attack_cameras.h"
#include "player_two/battle_camera.h"
#include "player_two/command_menu_two.h"
#include "player_two/focus_scope.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_on.h"
#include "player_two/party_member.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::field_camera {

namespace {

/// FEndFieldCameraOperator Activate/Deactivate (vtable +0x10/+0x20), CopyCommand (+0x58), and
/// AEndPlayerCameraManager::UpdateCamera (slot 211).
using StateFn  = void (*)(void* fieldOperator, float blendTime, bool flag);
using CopyFn   = void (*)(void* fieldOperator, const void* command);
using UpdateFn = void (*)(void* cameraManager, float deltaTime);

// Set once at install.
platform::VTableEntry g_activateSlot;
platform::VTableEntry g_deactivateSlot;
platform::VTableEntry g_copySlot;
platform::VTableEntry g_updateSlot;
StateFn               g_engineActivate      = nullptr;
StateFn               g_engineDeactivate    = nullptr;
CopyFn                g_engineCopy          = nullptr;
UpdateFn              g_engineUpdate        = nullptr;
std::uintptr_t        g_fieldVTable         = 0;
std::uintptr_t        g_cameraManagerVTable = 0;
using facts::kCmdHudMode;
std::atomic<unsigned> g_uiRefusals{0};  // battle updates that ran without player 2's UI context

// UpdateCamera's call of the Field operator's Activate when a special camera (a shop's) first blends out: the resume
// before it gave the Field operator that camera's last rotation (RE-p2-shop-camera-skip.md §5, §6). Its return address,
// this build (0 when not found): logged, to settle whether the mirror below snaps player 2's camera to it.
constexpr std::uintptr_t kBlendOutResume = 0x140E5629C;
std::uintptr_t           g_blendOutResume = 0;

StageCounters              g_settingsCounters;
StageCounters              g_updateCounters;
std::atomic<unsigned>      g_mismatches{0};

/// Player 1's and player 2's Field operators.
struct FieldPair
{
    void* one;
    void* two;
};

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

void* FieldOperatorOf(void* cameraManager) noexcept
{
    void* field = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    return VTableOf(field) == g_fieldVTable ? field : nullptr;
}

/// Both Field operators, while player 2 holds a party member.
std::optional<FieldPair> CurrentPair() noexcept
{
    if (!generation::OnGameThread() || !generation::Active() || generation::Member() == nullptr)
    {
        return std::nullopt;
    }
    void* one = FieldOperatorOf(CameraManagerOf(generation::PlayerOneController()));
    void* two = FieldOperatorOf(CameraManagerOf(generation::PlayerTwoController()));
    if (one == nullptr || two == nullptr || one == two)
    {
        return std::nullopt;
    }
    return FieldPair{one, two};
}

const void* Command(const void* field) noexcept
{
    return static_cast<const std::byte*>(field) + facts::kFieldOperatorCommand;
}

std::uint8_t ActiveState(const void* field) noexcept
{
    return ReadField<std::uint8_t>(field, facts::kFieldOperatorActive);
}

bool Matches(const FieldPair& pair) noexcept
{
    return std::memcmp(Command(pair.two), Command(pair.one), facts::kFieldCommandSize) == 0 &&
           ActiveState(pair.two) == ActiveState(pair.one);
}

void CountResult(const FieldPair& pair) noexcept
{
    g_settingsCounters.Act();
    if (!Matches(pair))
    {
        g_mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

// The three Field-operator replacements: the engine's call first, then the same on player 2's
// operator when `field` is player 1's.

void Activate(void* field, float blendTime, bool flag)
{
    g_engineActivate(field, blendTime, flag);
    const auto pair = CurrentPair();
    if (!pair || field != pair->one)
    {
        g_settingsCounters.Pass();
        return;
    }
    const auto twoWas = ActiveState(pair->two);
    g_engineActivate(pair->two, blendTime, flag);
    g_engineCopy(pair->two, Command(pair->one));  // after activation, which rewrites the command
    CountResult(*pair);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (g_blendOutResume != 0 && caller == g_blendOutResume)
    {
        platform::LogDebug(L"field camera: player 1's Field operator resumed after a special camera's blend-out (blend {:.2f} "
                           L"s); player 2's mirrored with player 1's command (player 2's operator state was {})",
                           blendTime, twoWas);
    }
}

void Deactivate(void* field, float blendTime, bool flag)
{
    g_engineDeactivate(field, blendTime, flag);
    const auto pair = CurrentPair();
    if (!pair || field != pair->one)
    {
        g_settingsCounters.Pass();
        return;
    }
    g_engineDeactivate(pair->two, blendTime, flag);
    CountResult(*pair);
}

void CopyCommand(void* field, const void* command)
{
    g_engineCopy(field, command);
    const auto pair = CurrentPair();
    if (!pair || field != pair->one)
    {
        g_settingsCounters.Pass();
        return;
    }
    g_engineCopy(pair->two, command);
    CountResult(*pair);
}

/// Player 2's camera manager, when its update can run as the primary player's.
bool ActsFor(void* cameraManager) noexcept
{
    if (!generation::OnGameThread() || !generation::Active() || generation::Member() == nullptr)
    {
        return false;
    }
    if (cameraManager != CameraManagerOf(generation::PlayerTwoController()))
    {
        return false;
    }
    // Its Field operator on, or in battle its Battle operator (player_two/battle_camera.h).
    void* field = FieldOperatorOf(cameraManager);
    if ((field == nullptr || ActiveState(field) == 0) && !battle_camera::PlayerTwoBattleActive(cameraManager))
    {
        return false;
    }
    // Helpers in the update index the operator array with the current-operator byte and use the
    // entry without a null check.
    const auto current = ReadField<std::uint8_t>(cameraManager, facts::kCameraManagerCurrentOperator);
    return current < facts::kCameraManagerOperatorCount &&
           ReadPointer(cameraManager, facts::kCameraManagerOperators + current * std::uint32_t{sizeof(void*)}) != nullptr;
}

/// When a scope cannot be opened, player 2's update still runs, as the engine would run it: with
/// player 1 as the primary player, so for that frame player 2's camera reads player 1's input.
/// Skipping the update instead would freeze player 2's camera. The refusal is counted.
///
/// In battle the update also runs in player 2's UI context: the Command camera module (21), which
/// player 2's command controller turns on for player 2's camera while its target list is open,
/// frames the command menu's selected target (EndUISystem[23]'s provider, which reads the selected
/// row from the focus path) and the party leader (RE-p2-target-select-views.md §6, §9). Without it,
/// player 1's target list would move player 2's camera.
void RunAsPrimary(void* cameraManager, float deltaTime)
{
    void*      controller = command_menu_two::Controller();
    void*      member     = generation::Member();
    const bool battle     = controller != nullptr && member != nullptr &&
                            ReadField<std::uint8_t>(controller, kCmdHudMode) != 0;
    std::optional<focus_scope::FocusScope> focus;
    if (battle)
    {
        focus.emplace(controller);
        if (!focus->Entered())
        {
            const unsigned refusals = g_uiRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
            if (refusals <= 5 || refusals % 600 == 0)
            {
                platform::Log(L"player 2's camera update ran without player 2's UI context: {} ({} time(s))",
                              focus->Refusal() != nullptr ? focus->Refusal() : L"refused", refusals);
            }
            focus.reset();
        }
    }
    const primary_controller::Scope scope;
    const lock_on::TargetScope      targets;  // in battle, the LockOn module reads player 2's displayed target
    std::optional<leader_scope::LeaderScope>     leader;
    std::optional<leader_scope::LeaderTypeScope> type;
    // In battle the leader view decides which camera an attack camera belongs to (the game's leader-keyed test of each
    // battle camera sequence, RE-p2-attack-cameras.md §3.2), so it opens even when the UI context was refused.
    if (focus || (member != nullptr && battle_camera::PlayerTwoBattleActive(cameraManager)))
    {
        const auto memberType = party_member::PlayerTypeOf(member);
        const auto memberWeak = leader_scope::WeakOf(member);
        if (memberType && memberWeak != 0)
        {
            leader.emplace(memberWeak);
            type.emplace(*memberType);
        }
    }
    if (scope.Held())
    {
        g_updateCounters.Act();
    }
    else
    {
        g_updateCounters.Refuse();
    }
    const attack_cameras::PlayerTwoUpdate attack;  // player 2's own state of the battle camera sequences (inside the scopes)
    g_engineUpdate(cameraManager, deltaTime);
}

void UpdateCamera(void* cameraManager, float deltaTime)
{
    if (!ActsFor(cameraManager))
    {
        g_updateCounters.Pass();
        g_engineUpdate(cameraManager, deltaTime);
        return;
    }
    RunAsPrimary(cameraManager, deltaTime);
}

bool CameraVTablesFound(const platform::GameImage& image) noexcept
{
    const auto field  = image.Rebase(facts::kFieldOperatorVTable);
    const auto camera = image.Rebase(facts::kCameraManagerVTable);
    if (!field || !camera)
    {
        return false;
    }
    g_fieldVTable         = *field;
    g_cameraManagerVTable = *camera;
    return true;
}

}  // namespace

bool InstallSettings(const platform::GameImage& image) noexcept
{
    const auto activate   = platform::FindFunction(image, facts::kFieldActivate.target);
    const auto deactivate = platform::FindFunction(image, facts::kFieldDeactivate.target);
    const auto copy       = platform::FindFunction(image, facts::kFieldCopyCommand.target);
    if (!activate || !deactivate || !copy || !CameraVTablesFound(image))
    {
        return false;
    }
    g_engineActivate   = reinterpret_cast<StateFn>(*activate);
    const auto resume  = image.Rebase(kBlendOutResume);
    g_blendOutResume   = resume ? *resume : 0;
    g_engineDeactivate = reinterpret_cast<StateFn>(*deactivate);
    g_engineCopy       = reinterpret_cast<CopyFn>(*copy);

    const bool installed =
        platform::ReplaceSlot(image, facts::kFieldActivate, reinterpret_cast<std::uintptr_t>(&Activate), g_activateSlot) &&
        platform::ReplaceSlot(image, facts::kFieldDeactivate, reinterpret_cast<std::uintptr_t>(&Deactivate),
                              g_deactivateSlot) &&
        platform::ReplaceSlot(image, facts::kFieldCopyCommand, reinterpret_cast<std::uintptr_t>(&CopyCommand),
                              g_copySlot);
    if (!installed)
    {
        // All three or none: a mirror of activation without the command would leave player 2's
        // operator re-initialised with no policy.
        g_activateSlot.Restore();
        g_deactivateSlot.Restore();
        g_copySlot.Restore();
    }
    return installed;
}

bool InstallUpdate(const platform::GameImage& image) noexcept
{
    const auto update = platform::FindFunction(image, facts::kCameraUpdate.target);
    if (!update || !CameraVTablesFound(image) || !primary_controller::Installed())
    {
        return false;
    }
    g_engineUpdate = reinterpret_cast<UpdateFn>(*update);
    return platform::ReplaceSlot(image, facts::kCameraUpdate, reinterpret_cast<std::uintptr_t>(&UpdateCamera),
                                 g_updateSlot);
}

void Remove() noexcept
{
    for (auto* slot : {&g_updateSlot, &g_copySlot, &g_deactivateSlot, &g_activateSlot})
    {
        if (!slot->Restore())
        {
            platform::Log(L"could not restore a player-2 camera slot; the replacement stays in place");
        }
    }
}

bool Seed() noexcept
{
    if (g_engineCopy == nullptr)
    {
        return false;
    }
    const auto pair = CurrentPair();
    if (!pair)
    {
        return false;
    }
    if (ActiveState(pair->two) != ActiveState(pair->one))
    {
        const StateFn apply = ActiveState(pair->one) != 0 ? g_engineActivate : g_engineDeactivate;
        apply(pair->two, 0.0f, false);
    }
    g_engineCopy(pair->two, Command(pair->one));
    return Matches(*pair);
}

StageCounters& SettingsCounters() noexcept
{
    return g_settingsCounters;
}

StageCounters& UpdateCounters() noexcept
{
    return g_updateCounters;
}

unsigned TakeMismatches() noexcept
{
    return platform::Take(g_mismatches);
}

}  // namespace lss::player_two::field_camera
