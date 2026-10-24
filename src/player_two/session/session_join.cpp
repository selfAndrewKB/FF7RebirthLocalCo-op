// Session: installing the stages, and player 2's join.

#include "player_two/session/session.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/log.h"
#include "player_two/camera/field_camera.h"
#include "player_two/camera/view_target.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/input/member_movement.h"
#include "player_two/input/move_state.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/object_fields.h"
#include "player_two/rides/shared_ride.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/scopes/scope_leaks.h"
#include "player_two/session/own_controller.h"
#include "player_two/session/party_events.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <vector>

namespace lss::player_two {

namespace {

using platform::Log;

/// The shared viewport's input-mode bytes (+0x361..+0x364), as one number for the log.
std::uint32_t InputModeOf(const void* localPlayer) noexcept
{
    const void* viewport = ReadPointer(localPlayer, facts::kLocalPlayerViewportClient);
    return viewport == nullptr ? 0 : ReadField<std::uint32_t>(viewport, facts::kViewportInputModeBytes);
}

/// How many objects of `className` (and subclasses) exist.
std::size_t CountObjects(const wchar_t* className) noexcept
{
    try
    {
        std::vector<RC::Unreal::UObject*> objects;
        RC::Unreal::UObjectGlobals::FindAllOf(className, objects);
        return objects.size();
    }
    catch (...)
    {
        return 0;  // only an allocation failure; a count is diagnostic
    }
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
        return {CountObjects(STR("HUD")), InputModeOf(playerOne)};
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
    const bool cameraUpdate  = installed_.primary && field_camera::InstallUpdate(image);
    installed_.padSticks     = installed_.primary && pad_sticks::Install(image);
    installed_.moveTicks     = move_state::Install(image);
    installed_.viewTarget    = view_target::Install(image);
    const bool scopeLeaks    = scope_leaks::Install(image);
    const bool padButtons    = pad_buttons::Install(image);
    installed_.memberMovement = installed_.member && installed_.padSticks && installed_.moveTicks &&
                                member_movement::Install(image);  // the member tick runs on the game's recorded ticks
    installed_.partyEvents =
        installed_.member &&
        party_events::Install(image, {[](void* self, void* controller, void* character) noexcept {
                                          static_cast<Session*>(self)->OnLeaderChanging(controller, character);
                                      },
                                      [](void* self, std::uint8_t type, std::uint8_t before, std::uint8_t after) noexcept {
                                          static_cast<Session*>(self)->OnStatusChanged(type, before, after);
                                      },
                                      [](void* self) noexcept { static_cast<Session*>(self)->OnPartyApplied(); },
                                      this});
    LogStage(L"own controller", installed_.ownController);
    LogStage(L"primary-controller scope", installed_.primary);
    LogStage(L"own party member", installed_.member);
    LogStage(L"own camera view", installed_.viewTarget);
    LogStage(L"field camera settings", installed_.fieldSettings);
    LogStage(L"own camera update", cameraUpdate);
    LogStage(L"per-pad sticks (on; Ctrl+F9 switches)", installed_.padSticks);
    LogStage(L"member movement (with per-pad sticks)", installed_.memberMovement);
    LogStage(L"move-state ticks (the game's recorded; player 1's on pad 1)", installed_.moveTicks);
    LogStage(L"scope leaks put back", scopeLeaks);
    LogStage(L"per-pad buttons (the split views; pad 2 kept from the menus)", padButtons);
    LogStage(L"party events (the game's leader and status changes)", installed_.partyEvents);
}

void Session::RemoveStages() noexcept
{
    party_events::Remove();
    member_movement::Remove();
    scope_leaks::Remove();
    pad_buttons::Remove();
    move_state::Remove();
    pad_sticks::Remove();
    field_camera::Remove();
    own_controller::Remove();
}

bool Seat::JoinAllowed(const players::LocalPlayers::PlayerOne& one) const noexcept
{
    if (refuseJoins_)
    {
        Log(L"join refused: an earlier join raised an exception inside the engine; restart the game to join again");
        return false;
    }
    if (const wchar_t* why = NotReadyToJoin())
    {
        Log(L"join refused: {}", why);
        return false;
    }
    if (one.controller == nullptr)
    {
        return false;  // FirstPlayer logged why
    }
    if (!own_controller::UsesNativeClasses(one.controller))
    {
        Log(L"join refused: player 1's controller or camera manager does not use the End classes' own vtable, so "
            L"player 2's camera manager could not be told apart safely");
        return false;
    }
    return true;
}

void Seat::Join() noexcept
{
    if (In())
    {
        Log(L"player 2 is already in");
        return;
    }
    const auto one = players_.FirstPlayer();
    if (!JoinAllowed(one))
    {
        return;
    }
    // Player 2 exists only with a character to play: with none free, the game stays as with one player. A shared ride
    // needs none: player 2 would stand aside in it anyway (the 2nd slot's member may sit in the vehicle on another AI
    // brain, as in a save loaded mid-ride), and takes the member once the ride is over.
    const bool riding = shared_ride::Rides(one.controller);
    if (installed_.member && !riding && FreeMember(one.controller) == nullptr)
    {
        party_member::ChooseSlot(seat_, one.controller);  // the 2nd slot's verdict, for the log
        AwaitMember(L"the party's 2nd slot has no member free for player 2");
        return;
    }
    if (riding && !sharedRide_)
    {
        sharedRide_ = true;  // before the first choice: player 2 takes no member until the ride is over
        Log(L"player 1 rides a vehicle the whole party shares: player 2 joins standing aside");
    }

    const auto                   before = JoinWitness::Take(one.localPlayer);
    players::LocalPlayers::Added added;
    own_controller::Report       report;
    {
        const own_controller::JoinScope scope(one.controller, seat_);
        added  = players_.Add(one.controller);
        report = scope.Result();
    }
    if (!In())
    {
        return;  // Add logged why
    }
    // Recorded before anything else, so any leave below works on the objects the engine created.
    playerOne_           = one.localPlayer;
    playerOneController_ = one.controller;
    controller_          = added.controller;
    phase_               = Phase::Playing;
    outside_.End();  // joined, by the key, the rejoin after a map load, or a member freed

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

void Seat::AimOwnCamera() noexcept
{
    if (!installed_.viewTarget)
    {
        return;
    }
    void* before = view_target::Current(controller_);
    const bool aimed = view_target::Aim(controller_);
    Log(L"player 2's camera manager viewed {}; after aiming it at its camera actor it views {} ({}); auto-managed "
        L"view target: player 1 {}, player 2 {}",
        NameOf(before), NameOf(view_target::Current(controller_)), aimed ? L"its own camera" : L"NOT its camera",
        static_cast<unsigned>(ReadField<std::uint8_t>(playerOneController_, facts::kControllerAutoManageCamera)),
        static_cast<unsigned>(ReadField<std::uint8_t>(controller_, facts::kControllerAutoManageCamera)));
}

void Seat::StartGeneration() noexcept
{
    generation::Begin(seat_, playerOneController_, controller_);
    if (installed_.primary && !primary_controller::Bind(seat_, playerOneController_, controller_))
    {
        generation::Fault(seat_, L"player 2's controller could not be named as a weak pointer");
    }
    phase_            = Phase::SettlingPlayerOne;
    ticks_            = 0;
    gateClosedLogged_ = false;
}

}  // namespace lss::player_two
