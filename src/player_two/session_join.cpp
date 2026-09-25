// Session: installing the stages, and player 2's join.

#include "player_two/session.h"

#include "platform/log.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/field_camera.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"
#include "player_two/move_state.h"
#include "player_two/object_fields.h"
#include "player_two/own_controller.h"
#include "player_two/pad_sticks.h"
#include "player_two/primary_controller.h"
#include "player_two/scope_leaks.h"
#include "player_two/tag_counts.h"
#include "player_two/view_target.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>

namespace lss::player_two {

namespace {

using platform::Log;

/// The shared viewport's input-mode bytes (+0x361..+0x364), as one number for the log.
std::uint32_t InputModeOf(const void* localPlayer) noexcept
{
    const void* viewport = ReadPointer(localPlayer, facts::kLocalPlayerViewportClient);
    return viewport == nullptr ? 0 : ReadField<std::uint32_t>(viewport, facts::kViewportInputModeBytes);
}

void LogStage(const wchar_t* stage, bool installed) noexcept
{
    Log(L"stage {}: {}", stage, installed ? L"installed" : L"NOT installed (its checks failed; see above)");
}

std::wstring ClassNameOf(void* object) noexcept
{
    if (object == nullptr)
    {
        return L"(none)";
    }
    try
    {
        return static_cast<RC::Unreal::UObject*>(object)->GetClassPrivate()->GetName();
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

/// The state around a join that the log compares before and after.
struct JoinWitness
{
    std::size_t   huds      = 0;
    std::uint32_t inputMode = 0;

    static JoinWitness Take(const void* playerOne) noexcept
    {
        return {players::unreal::CountObjects(STR("HUD")), InputModeOf(playerOne)};
    }
};

/// Player 2's controller has a camera manager that began play completely: owned, with its camera.
bool CameraManagerComplete(void* controller) noexcept
{
    void* cameraManager = ReadPointer(controller, facts::kControllerCameraManager);
    return cameraManager != nullptr && ReadPointer(cameraManager, facts::kCameraManagerPcOwner) == controller &&
           ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor) != nullptr;
}

void LogJoined(void* two, void* one, const own_controller::Report& report, const JoinWitness& before,
               const JoinWitness& after) noexcept
{
    void* cameraManager = ReadPointer(two, facts::kControllerCameraManager);
    Log(L"player 2 joined with its own controller: {} (class {}), camera manager handled here: {} (owner filled: {}, "
        L"names swapped: {}), camera actor '{}' (player 1's: '{}'); HUDs {} -> {}; viewport input mode {:#010x} -> "
        L"{:#010x}",
        NameOf(two), ClassNameOf(two), YesNo(cameraManager != nullptr && cameraManager == report.cameraManager),
        YesNo(report.ownerFilled), YesNo(report.namesSwapped),
        NameOf(ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor)),
        NameOf(ReadPointer(ReadPointer(one, facts::kControllerCameraManager), facts::kCameraManagerEndCameraActor)),
        before.huds, after.huds, before.inputMode, after.inputMode);
}

}  // namespace

void Session::InstallStages(const platform::GameImage& image) noexcept
{
    installed_.ownController = own_controller::Install(image);
    installed_.primary       = primary_controller::Install(image);
    installed_.member        = installed_.primary && party_member::Install(image);
    installed_.fieldSettings = field_camera::InstallSettings(image);
    installed_.cameraUpdate  = installed_.primary && field_camera::InstallUpdate(image);
    installed_.padSticks     = installed_.primary && pad_sticks::Install(image);
    installed_.moveCensus    = move_state::Install(image);
    installed_.tagCounts     = tag_counts::Install(image);
    installed_.viewTarget    = view_target::Install(image);
    installed_.scopeLeaks    = scope_leaks::Install(image);
    installed_.memberMovement = installed_.member && installed_.padSticks && installed_.moveCensus &&
                                member_movement::Install(image);
    LogStage(L"own controller", installed_.ownController);
    LogStage(L"primary-controller scope", installed_.primary);
    LogStage(L"own party member", installed_.member);
    LogStage(L"own camera view", installed_.viewTarget);
    LogStage(L"field camera settings", installed_.fieldSettings);
    LogStage(L"own camera update", installed_.cameraUpdate);
    LogStage(L"per-pad sticks (off until Ctrl+F9)", installed_.padSticks);
    LogStage(L"member movement (with per-pad sticks)", installed_.memberMovement);
    LogStage(L"movement census", installed_.moveCensus);
    LogStage(L"tag counts (log only)", installed_.tagCounts);
    LogStage(L"scope leaks put back", installed_.scopeLeaks);
}

void Session::RemoveStages() noexcept
{
    member_movement::Remove();
    scope_leaks::Remove();
    move_state::Remove();
    pad_sticks::Remove();
    field_camera::Remove();
    own_controller::Remove();
}

bool Session::JoinAllowed(const players::LocalPlayers::PlayerOne& one) const noexcept
{
    if (refuseJoins_)
    {
        Log(L"join refused: an earlier join raised an exception inside the engine; restart the game to join again");
        return false;
    }
    if (!installed_.ownController || !own_controller::Ready())
    {
        Log(L"join refused: player 2's camera manager cannot be started safely yet ({})",
            installed_.ownController ? L"player 1's camera names are not set yet" : L"its stage is not installed");
        return false;
    }
    if (installed_.member && !party_member::FieldGateOpen())
    {
        Log(L"join refused: not in the field (player 2 joins in the field only)");
        return false;
    }
    if (one.controller == nullptr)
    {
        return false;  // SolePlayer logged why
    }
    if (!own_controller::UsesNativeClasses(one.controller))
    {
        Log(L"join refused: player 1's controller or camera manager does not use the End classes' own vtable, so "
            L"player 2's camera manager could not be told apart safely");
        return false;
    }
    return true;
}

void Session::Join() noexcept
{
    if (HasPlayerTwo())
    {
        Log(L"player 2 is already in");
        return;
    }
    const auto one = players_.SolePlayer();
    if (!JoinAllowed(one))
    {
        return;
    }

    const auto                   before = JoinWitness::Take(one.localPlayer);
    players::LocalPlayers::Added added;
    own_controller::Report       report;
    {
        const own_controller::JoinScope scope(one.controller);
        added  = players_.Add(one.controller);
        report = scope.Result();
    }
    if (!HasPlayerTwo())
    {
        return;  // Add logged why
    }
    // Recorded before anything else, so any leave below works on the objects the engine created.
    playerOne_           = one.localPlayer;
    playerOneController_ = one.controller;
    playerTwoController_ = added.controller;
    phase_               = Phase::Playing;

    if (added.raised)
    {
        refuseJoins_ = true;  // never carry on with a half-built player
        Log(L"JOIN FAILED: the engine raised an exception while creating player 2 (camera manager handled: {}, owner "
            L"filled: {}, owner refused: {}, names swapped: {}). Player 2 is being removed; no further joins this "
            L"session.",
            YesNo(report.cameraManager != nullptr), YesNo(report.ownerFilled), YesNo(report.ownerRefused),
            YesNo(report.namesSwapped));
        Leave();
        return;
    }
    LogJoined(added.controller, one.controller, report, before, JoinWitness::Take(one.localPlayer));
    if (!CameraManagerComplete(added.controller))
    {
        Log(L"player 2's controller or camera manager is incomplete; player 2 stays on player 1's view and takes no "
            L"party member (Ctrl+F8 removes player 2)");
        return;
    }
    StartGeneration();
    AimOwnCamera();
}

void Session::AimOwnCamera() noexcept
{
    if (!installed_.viewTarget)
    {
        return;
    }
    void* before = view_target::Current(playerTwoController_);
    const bool aimed = view_target::Aim(playerTwoController_);
    Log(L"player 2's camera manager viewed {}; after aiming it at its camera actor it views {} ({}); auto-managed "
        L"view target: player 1 {}, player 2 {}",
        NameOf(before), NameOf(view_target::Current(playerTwoController_)), aimed ? L"its own camera" : L"NOT its camera",
        static_cast<unsigned>(ReadField<std::uint8_t>(playerOneController_, facts::kControllerAutoManageCamera)),
        static_cast<unsigned>(ReadField<std::uint8_t>(playerTwoController_, facts::kControllerAutoManageCamera)));
}

void Session::StartGeneration() noexcept
{
    generation::Begin(playerOneController_, playerTwoController_);
    if (installed_.primary && !primary_controller::Bind(playerOneController_, playerTwoController_))
    {
        generation::Fault(L"player 2's controller could not be named as a weak pointer");
    }
    phase_            = Phase::SettlingPlayerOne;
    ticks_            = 0;
    gateClosedLogged_ = false;
}

}  // namespace lss::player_two
