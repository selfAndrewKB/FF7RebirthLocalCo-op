// Session: installing the stages, and a seat's join.

#include "session/session.h"

#include "camera/field_camera.h"
#include "camera/view_target.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "input/member_movement.h"
#include "input/move_state.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/log.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "rides/shared_ride.h"
#include "scopes/primary_controller.h"
#include "scopes/scope_leaks.h"
#include "session/lead_gate.h"
#include "session/own_controller.h"
#include "session/party_events.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <vector>

namespace lss::session {

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

/// A seat's controller has a camera manager that began play completely: owned, with its camera.
bool CameraManagerComplete(void* controller) noexcept
{
    void* cameraManager = ReadPointer(controller, facts::kControllerCameraManager);
    return cameraManager != nullptr && ReadPointer(cameraManager, facts::kCameraManagerPcOwner) == controller &&
           ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor) != nullptr;
}

void LogJoined(const wchar_t* name, void* seatController, void* one, const own_controller::Report& report,
               const JoinWitness& before, const JoinWitness& after) noexcept
{
    void* cameraManager = ReadPointer(seatController, facts::kControllerCameraManager);
    Log(L"{} joined with its own controller: {} (class {}), camera manager handled here: {} (owner filled: {}, "
        L"names swapped: {}), camera actor '{}' (player 1's: '{}'); HUDs {} -> {}; viewport input mode {:#010x} -> "
        L"{:#010x}",
        name, players::NameOf(seatController), ClassNameOf(seatController),
        players::YesNo(cameraManager != nullptr && cameraManager == report.cameraManager),
        players::YesNo(report.ownerFilled), players::YesNo(report.namesSwapped),
        players::NameOf(ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor)),
        players::NameOf(ReadPointer(ReadPointer(one, facts::kControllerCameraManager), facts::kCameraManagerEndCameraActor)),
        before.huds, after.huds, before.inputMode, after.inputMode);
}

}  // namespace

void Session::InstallStages(const platform::GameImage& image) noexcept
{
    installed_.ownController = own_controller::Install(image);
    installed_.primary       = scopes::primary_controller::Install(image);
    installed_.member        = installed_.primary && party_member::Install(image);
    installed_.fieldSettings = camera::field_camera::InstallSettings(image);
    const bool cameraUpdate  = installed_.primary && camera::field_camera::InstallUpdate(image);
    installed_.padSticks     = installed_.primary && input::pad_sticks::Install(image);
    installed_.moveTicks     = input::move_state::Install(image);
    installed_.viewTarget    = camera::view_target::Install(image);
    const bool scopeLeaks    = scopes::scope_leaks::Install(image);
    const bool padButtons    = input::pad_buttons::Install(image);
    installed_.memberMovement = installed_.member && installed_.padSticks && installed_.moveTicks &&
                                input::member_movement::Install(image);  // the member tick runs on the game's recorded ticks
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
    LogStage(L"per-pad buttons (the split views; pads 2 and 3 kept from the menus)", padButtons);
    LogStage(L"party events (the game's leader and status changes)", installed_.partyEvents);
    LogStage(L"lead gate (a level's walk nothing needs stays the player's)", lead_gate::Install(image));
}

void Session::RemoveStages() noexcept
{
    party_events::Remove();
    input::member_movement::Remove();
    scopes::scope_leaks::Remove();
    input::pad_buttons::Remove();
    input::move_state::Remove();
    input::pad_sticks::Remove();
    camera::field_camera::Remove();
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
            L"{}'s camera manager could not be told apart safely",
            Name());
        return false;
    }
    return true;
}

void Seat::Join() noexcept
{
    if (In())
    {
        Log(L"{} is already in", Name());
        return;
    }
    const auto one = players_.FirstPlayer();
    if (!JoinAllowed(one))
    {
        return;
    }
    // A seat's player exists only with a character to play: with none free, the game stays as without them. A shared
    // ride needs none: the player would stand aside in it anyway (the slot's member may sit in the vehicle on another
    // AI brain, as in a save loaded mid-ride), and takes the member once the ride is over.
    const bool riding = rides::shared_ride::Rides(one.controller);
    if (installed_.member && !riding && FreeMember(one.controller) == nullptr)
    {
        party_member::ChooseSlot(seat_, one.controller, standIn_);  // the slot's verdict, for the log
        AwaitMember(L"the player's slot of the party has no member free");
        return;
    }
    if (riding && !sharedRide_)
    {
        sharedRide_ = true;  // before the first choice: the seat takes no member until the ride is over
        Log(L"player 1 rides a vehicle the whole party shares: {} joins standing aside", Name());
    }

    // The mirror views out first: the engine appends the seat's player after whoever is listed, and its column must
    // follow the players'. They come back on this tick, after the session's (modules.cpp).
    if (installed_.clearMirrors != nullptr && !installed_.clearMirrors())
    {
        Log(L"join refused: a mirror view could not be taken out of the engine's list (the player would land after it); "
            L"tried again with the next join");
        return;
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
        Log(L"JOIN FAILED: the engine raised an exception while creating {} (camera manager handled: {}, owner "
            L"filled: {}, owner refused: {}, names swapped: {}). The player is being removed; no further joins this "
            L"session.",
            Name(), players::YesNo(report.cameraManager != nullptr), players::YesNo(report.ownerFilled), players::YesNo(report.ownerRefused),
            players::YesNo(report.namesSwapped));
        Leave();
        return;
    }
    LogJoined(Name(), added.controller, one.controller, report, before, JoinWitness::Take(one.localPlayer));
    if (!CameraManagerComplete(added.controller))
    {
        Log(L"{}'s controller or camera manager is incomplete; the player stays on player 1's view and takes no "
            L"party member (Ctrl+F8 removes them)",
            Name());
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
    void* before = camera::view_target::Current(controller_);
    const bool aimed = camera::view_target::Aim(controller_);
    Log(L"{}'s camera manager viewed {}; after aiming it at its camera actor it views {} ({}); auto-managed "
        L"view target: player 1 {}, the joining player {}",
        Name(), players::NameOf(before), players::NameOf(camera::view_target::Current(controller_)), aimed ? L"its own camera" : L"NOT its camera",
        static_cast<unsigned>(ReadField<std::uint8_t>(playerOneController_, facts::kControllerAutoManageCamera)),
        static_cast<unsigned>(ReadField<std::uint8_t>(controller_, facts::kControllerAutoManageCamera)));
}

void Seat::StartGeneration() noexcept
{
    players::generation::Begin(seat_, playerOneController_, controller_);
    if (installed_.primary && !scopes::primary_controller::Bind(seat_, playerOneController_, controller_))
    {
        players::generation::Fault(seat_, L"the player's controller could not be named as a weak pointer");
    }
    phase_            = Phase::SettlingPlayerOne;
    ticks_            = 0;
    gateClosedLogged_ = false;
}

}  // namespace lss::session
