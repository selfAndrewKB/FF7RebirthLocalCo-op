#include "camera/field_camera.h"

#include "battle/battle_camera.h"
#include "battle/command_menu.h"
#include "battle/focus_scope.h"
#include "battle/lock_on.h"
#include "camera/attack_cameras.h"
#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/command_controller.h"
#include "facts/controller.h"
#include "platform/every.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::camera::field_camera {

namespace seat_ladder = scopes::seat_ladder;

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
using facts::kCmdHudMode;
std::atomic<unsigned> g_uiRefusals{0};  // battle updates that ran without the seat's UI context

// UpdateCamera's call of the Field operator's Activate when a special camera (a shop's) first blends out: the resume
// before it gave the Field operator that camera's last rotation (RE-p2-shop-camera-skip.md §5, §6). Its return address,
// this build (0 when not found): logged, to settle whether the mirror below snaps a seat's camera to it.
constexpr std::uintptr_t kBlendOutResume = 0x140E5629C;
std::uintptr_t           g_blendOutResume = 0;

// By seat; index 0 (player 1) unused.
std::array<players::StageCounters, players::kMaxPlayers>         g_settingsCounters{};
std::array<players::StageCounters, players::kMaxPlayers>         g_updateCounters{};
std::array<std::atomic<unsigned>, players::kMaxPlayers> g_mismatches{};
std::array<std::atomic<unsigned>, players::kMaxPlayers> g_notMirrored{};  // commands under a cutscene camera
bool                                                    g_underOther = false;  // as last logged (game thread)

/// Player 1's and `seat`'s Field operators, while the seat's player holds a party member
/// (camera/camera_access.h).
using FieldPair = camera_access::OperatorPair;
using camera_access::CameraManagerOf;
using camera_access::FieldOperatorOf;

std::optional<FieldPair> CurrentPair(int seat) noexcept
{
    return camera_access::CurrentPair(seat, &FieldOperatorOf);
}

std::size_t Index(int seat) noexcept
{
    return static_cast<std::size_t>(seat);
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
    return std::memcmp(Command(pair.seatOperator), Command(pair.operatorOne), facts::kFieldCommandSize) == 0 &&
           ActiveState(pair.seatOperator) == ActiveState(pair.operatorOne);
}

/// Player 1's Field operator is the top operator of player 1's camera manager, or the Battle operator is (the field
/// camera's commands still reach it under the battle). Under a cutscene or special camera the cut's own
/// tick pushes its rotation into player 1's Field command every frame (RE-p2-shop-camera-skip.md section 12); those
/// commands are the cut's, not the field's, so they are not mirrored: mirrored, they pinned the seat's camera to the
/// cut's rotation, and the copy at the cut's end turned it to player 1's angle (seen in play). Read once
/// per engine call, after CurrentPair found player 1's camera manager (game thread). Logged on change.
bool PlayerOneFieldCommanded() noexcept
{
    void* pcm = CameraManagerOf(players::generation::ControllerOf(0));
    const auto current =
        pcm != nullptr ? ReadField<std::uint8_t>(pcm, facts::kCameraManagerCurrentOperator) : facts::kFieldOperatorIndex;
    const bool commanded = current == facts::kFieldOperatorIndex || current == facts::kBattleOperatorIndex;
    if (g_underOther == commanded)
    {
        g_underOther = !commanded;
        if (commanded)
        {
            platform::LogDebug(L"field camera: player 1's Field operator seen on top of its camera manager again (or "
                               L"the battle's): the game's commands to it are mirrored to the other players' again");
        }
        else
        {
            platform::LogDebug(L"field camera: player 1's camera manager has its operator {} on top (a cutscene or "
                               L"special camera): the game's commands to player 1's Field operator are not mirrored to "
                               L"the other players' until the Field operator is on top again",
                               current);
        }
    }
    return commanded;
}

void CountResult(int seat, const FieldPair& pair) noexcept
{
    g_settingsCounters[Index(seat)].Act();
    if (!Matches(pair))
    {
        g_mismatches[Index(seat)].fetch_add(1, std::memory_order_relaxed);
    }
}

/// Under a cut only the state is mirrored, so only the state is compared.
void CountState(int seat, const FieldPair& pair) noexcept
{
    g_settingsCounters[Index(seat)].Act();
    if (ActiveState(pair.seatOperator) != ActiveState(pair.operatorOne))
    {
        g_mismatches[Index(seat)].fetch_add(1, std::memory_order_relaxed);
    }
}

// The three Field-operator replacements: the engine's call first, then the same on each seat's
// operator, in seat order, when `field` is player 1's.

void Activate(void* field, float blendTime, bool flag)
{
    g_engineActivate(field, blendTime, flag);
    const auto          caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::optional<bool> commanded;  // once per engine call, at the first seat
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto pair = CurrentPair(seat);
        if (!pair || field != pair->operatorOne)
        {
            g_settingsCounters[Index(seat)].Pass();
            continue;
        }
        if (!commanded)
        {
            commanded = PlayerOneFieldCommanded();
        }
        const auto seatWas = ActiveState(pair->seatOperator);
        g_engineActivate(pair->seatOperator, blendTime, flag);
        if (*commanded)
        {
            // After activation: the first one writes command defaults.
            g_engineCopy(pair->seatOperator, Command(pair->operatorOne));
            CountResult(seat, *pair);
        }
        else
        {
            CountState(seat, *pair);  // the activation alone: under a cut the command is the seat's own
        }
        if (g_blendOutResume != 0 && caller == g_blendOutResume)
        {
            platform::LogDebug(L"field camera: player 1's Field operator resumed after a special camera's blend-out "
                               L"(blend {:.2f} s); {}'s activated with it, its command {} ({}'s operator state was {})",
                               blendTime, players::SeatName(seat), *commanded ? L"player 1's" : L"its own",
                               players::SeatName(seat), seatWas);
        }
    }
}

void Deactivate(void* field, float blendTime, bool flag)
{
    g_engineDeactivate(field, blendTime, flag);
    std::optional<bool> commanded;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto pair = CurrentPair(seat);
        if (!pair || field != pair->operatorOne)
        {
            g_settingsCounters[Index(seat)].Pass();
            continue;
        }
        g_engineDeactivate(pair->seatOperator, blendTime, flag);
        if (!commanded)
        {
            commanded = PlayerOneFieldCommanded();
        }
        if (*commanded)
        {
            CountResult(seat, *pair);
        }
        else
        {
            CountState(seat, *pair);  // under a cut player 1's command is the cut's: not compared
        }
    }
}

void CopyCommand(void* field, const void* command)
{
    g_engineCopy(field, command);
    std::optional<bool> commanded;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto pair = CurrentPair(seat);
        if (!pair || field != pair->operatorOne)
        {
            g_settingsCounters[Index(seat)].Pass();
            continue;
        }
        if (!commanded)
        {
            commanded = PlayerOneFieldCommanded();
        }
        if (!*commanded)
        {
            g_notMirrored[Index(seat)].fetch_add(1, std::memory_order_relaxed);  // a cut's per-frame rotation push
            g_settingsCounters[Index(seat)].Pass();
            continue;
        }
        g_engineCopy(pair->seatOperator, command);
        CountResult(seat, *pair);
    }
}

/// The seat whose camera manager `cameraManager` is, when its update can run as the primary player's; 0 otherwise.
int ActsFor(void* cameraManager) noexcept
{
    if (!players::generation::OnGameThread())
    {
        return 0;
    }
    const int seat = camera_access::SeatOfCameraManager(cameraManager);  // active seats only
    if (!players::SeatBuilt(seat) || players::generation::MemberOf(seat) == nullptr ||
        cameraManager != CameraManagerOf(players::generation::ControllerOf(seat)))
    {
        return 0;
    }
    // Its Field operator on, or in battle its Battle operator (battle/battle_camera.h).
    void* field = FieldOperatorOf(cameraManager);
    if ((field == nullptr || ActiveState(field) == 0) && !battle::battle_camera::SeatBattleActive(cameraManager))
    {
        return 0;
    }
    // Helpers in the update index the operator array with the current-operator byte and use the
    // entry without a null check.
    const auto current = ReadField<std::uint8_t>(cameraManager, facts::kCameraManagerCurrentOperator);
    const bool operatorThere =
        current < facts::kCameraManagerOperatorCount &&
        ReadPointer(cameraManager, facts::kCameraManagerOperators + current * std::uint32_t{sizeof(void*)}) != nullptr;
    return operatorThere ? seat : 0;
}

/// When a scope cannot be opened, the seat's update still runs, as the engine would run it: with
/// player 1 as the primary player, so for that frame the seat's camera reads player 1's input.
/// Skipping the update instead would freeze the seat's camera. The refusal is counted.
///
/// In battle the update also runs in the seat's UI context: the Command camera module (21), which
/// the seat's command controller turns on for the seat's camera while its target list is open,
/// frames the command menu's selected target (EndUISystem[23]'s provider, which reads the selected
/// row from the focus path) and the party leader (RE-p2-target-select-views.md §6, §9). Without it,
/// player 1's target list would move the seat's camera.
void RunAsPrimary(int seat, void* cameraManager, float deltaTime)
{
    void*      controller = battle::command_menu::Controller(seat);
    void*      member     = players::generation::MemberOf(seat);
    const bool battle     = controller != nullptr && member != nullptr &&
                            ReadField<std::uint8_t>(controller, kCmdHudMode) != 0;
    // In battle the seat's UI context (counted and logged when refused), the seat as the primary controller (refused:
    // the update runs as player 1's, counted) and the target fields the LockOn module reads: each as far as it opens,
    // the update runs either way.
    seat_ladder::Args args;
    args.seat            = seat;
    args.controller      = controller;
    const unsigned which = (battle ? seat_ladder::Focus : 0u) | seat_ladder::Primary | seat_ladder::Targets;
    const seat_ladder::SeatScopes scopes(which, which, args);
    if (battle && !scopes.Holds<seat_ladder::FocusPart>())
    {
        const unsigned refusals = g_uiRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (refusals <= 5 || refusals % 600 == 0)
        {
            platform::Log(L"{}'s camera update ran without its UI context: {} ({} time(s))", players::SeatName(seat),
                          scopes.Noted() != nullptr ? scopes.Noted() : L"refused", refusals);
        }
    }
    // In battle the leader view decides which camera an attack camera belongs to (the game's leader-keyed test of each
    // battle camera sequence, RE-p2-attack-cameras.md §3.2), so it opens even when the UI context was refused.
    std::optional<seat_ladder::SeatScopes> leader;
    if (scopes.Holds<seat_ladder::FocusPart>() ||
        (member != nullptr && battle::battle_camera::SeatBattleActive(cameraManager)))
    {
        if (member != nullptr && seat_ladder::FillMember(args, member))
        {
            leader.emplace(seat_ladder::Leader | seat_ladder::LeaderType, seat_ladder::Leader | seat_ladder::LeaderType,
                           args);
        }
    }
    if (scopes.Holds<seat_ladder::PrimaryPart>())
    {
        g_updateCounters[Index(seat)].Act();
    }
    else
    {
        g_updateCounters[Index(seat)].Refuse();
    }
    const attack_cameras::SeatUpdate attack(seat);  // the seat's own sequence state (inside the scopes)
    g_engineUpdate(cameraManager, deltaTime);
}

void UpdateCamera(void* cameraManager, float deltaTime)
{
    const int seat = ActsFor(cameraManager);
    for (int other = 1; other <= players::kSeatsBuilt; ++other)
    {
        if (other != seat)
        {
            g_updateCounters[Index(other)].Pass();  // not this seat's update: passed, from that seat's view
        }
    }
    if (seat == 0)
    {
        g_engineUpdate(cameraManager, deltaTime);
        return;
    }
    RunAsPrimary(seat, cameraManager, deltaTime);
}

}  // namespace

bool InstallSettings(const platform::GameImage& image) noexcept
{
    const auto activate   = platform::FindFunction(image, facts::kFieldActivate.target);
    const auto deactivate = platform::FindFunction(image, facts::kFieldDeactivate.target);
    const auto copy       = platform::FindFunction(image, facts::kFieldCopyCommand.target);
    if (!activate || !deactivate || !copy || !camera_access::Install(image))
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
        // All three or none: the activation, the deactivation and the command are one setup, mirrored whole or not.
        g_activateSlot.Restore();
        g_deactivateSlot.Restore();
        g_copySlot.Restore();
    }
    return installed;
}

bool InstallUpdate(const platform::GameImage& image) noexcept
{
    const auto update = platform::FindFunction(image, facts::kCameraUpdate.target);
    if (!update || !camera_access::Install(image) || !scopes::primary_controller::Installed())
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
    g_underOther = false;
}

Seeded Seed(int seat) noexcept
{
    if (g_engineCopy == nullptr)
    {
        return Seeded::NoPair;
    }
    const auto pair = CurrentPair(seat);
    if (!pair)
    {
        return Seeded::NoPair;
    }
    if (ActiveState(pair->seatOperator) != ActiveState(pair->operatorOne))
    {
        const StateFn apply = ActiveState(pair->operatorOne) != 0 ? g_engineActivate : g_engineDeactivate;
        apply(pair->seatOperator, 0.0f, false);
    }
    if (!PlayerOneFieldCommanded())
    {
        return Seeded::PlayerOneUnderOther;  // player 1's command is the cut's this frame: the seat keeps its own
    }
    g_engineCopy(pair->seatOperator, Command(pair->operatorOne));
    return Matches(*pair) ? Seeded::Matches : Seeded::Mismatch;
}

const wchar_t* Describe(Seeded seeded) noexcept
{
    switch (seeded)
    {
        case Seeded::NoPair: return L"no operator pair (not installed, or the operators are not distinct)";
        case Seeded::Matches: return L"matches";
        case Seeded::Mismatch: return L"DOES NOT MATCH";
        case Seeded::PlayerOneUnderOther:
            return L"player 1's camera is under a cutscene or special camera: the state brought over, the command the "
                   L"player's own until the cut ends";
    }
    return L"?";
}

/// The index of `seat`'s counters; a number that is no built seat's reads the first seat's, never out of range.
std::size_t CounterIndex(int seat) noexcept
{
    return Index(players::SeatBuilt(seat) ? seat : 1);
}

unsigned TakeNotMirrored(int seat) noexcept
{
    return platform::Take(g_notMirrored[CounterIndex(seat)]);
}

players::StageCounters& SettingsCounters(int seat) noexcept
{
    return g_settingsCounters[CounterIndex(seat)];
}

players::StageCounters& UpdateCounters(int seat) noexcept
{
    return g_updateCounters[CounterIndex(seat)];
}

unsigned TakeMismatches(int seat) noexcept
{
    return platform::Take(g_mismatches[CounterIndex(seat)]);
}

}  // namespace lss::camera::field_camera
