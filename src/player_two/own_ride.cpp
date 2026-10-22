#include "player_two/own_ride.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/camera_access.h"
#include "player_two/chocobo_ride.h"
#include "player_two/describe.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/wheelie_ride.h"

#include <format>

namespace lss::player_two::own_ride {

namespace {

using facts::kCameraManagerFieldOperator;
using facts::kCharacterControlHolder;
using facts::kControllerActiveVehicle;
constexpr std::uint32_t kHolderComponent = 0x40;
constexpr std::uint32_t kModuleSlot15    = 15;

using GetterFn = void* (*)(const void* object);

struct RestoreCall
{
    void*      member    = nullptr;
    void*      playerTwo = nullptr;
    SwitchAiFn switchAi  = nullptr;
    bool       ok        = false;
};

void RunRestore(void* context)
{
    auto* call = static_cast<RestoreCall*>(context);
    call->ok   = platform::CallGuarded(call->switchAi, call->member, std::uint8_t{1}, call->playerTwo);
}

}  // namespace

std::uintptr_t FindOrZero(const platform::GameImage& image, const platform::FunctionFact& fact) noexcept
{
    const auto live = platform::FindFunction(image, fact);
    return live ? *live : std::uintptr_t{0};
}

std::uintptr_t RebaseOrZero(const platform::GameImage& image, std::uintptr_t ida) noexcept
{
    const auto live = image.Rebase(ida);
    return live ? *live : std::uintptr_t{0};
}

std::int32_t CoordState(const void* coordinator, std::uintptr_t coordVt, std::uint32_t rowField) noexcept
{
    const void* row = VTableOf(coordinator) == coordVt ? ReadPointer(coordinator, rowField) : nullptr;
    return row != nullptr ? ReadField<std::int32_t>(row, 0) : -1;
}

const void* CameraModule15(const void* playerTwo) noexcept
{
    const void* pcm     = ReadPointer(playerTwo, facts::kControllerCameraManager);
    const void* op      = ReadPointer(pcm, kCameraManagerFieldOperator);
    return camera_access::ModuleAt(op, kModuleSlot15);
}

const void* InputComponentOf(const void* vehicle) noexcept
{
    const void* holder = ReadPointer(vehicle, kCharacterControlHolder);
    const void* vt     = reinterpret_cast<const void*>(VTableOf(holder));
    const auto  getter = vt != nullptr ? ReadField<GetterFn>(vt, kHolderComponent) : nullptr;
    void*       found  = nullptr;
    return getter != nullptr && platform::CallGuarded(getter, found, holder) ? found : nullptr;
}

bool PlayerTwoHolds(const void* character, const void* playerTwo) noexcept
{
    return ReadPointer(character, facts::kCharacterEffectiveController) == playerTwo &&
           ReadPointer(playerTwo, facts::kControllerPawn) == character;
}

bool BoardingGateOpen(void* member, void* playerTwo, const wchar_t* label) noexcept
{
    const auto gate = member_movement::PlayerCoreAsPlayerTwo(member, member);
    if (gate.answer == 1 && gate.primary == playerTwo)
    {
        return true;
    }
    platform::Log(L"{}: not boarded (the boarding's player-core test as player 2 reads {}, primary {})", label,
                  gate.answer, gate.primary == playerTwo ? L"PC2" : L"not PC2");
    return false;
}

std::wstring PossessionText(const Lease& lease, const wchar_t* word)
{
    const void* pawn   = ReadPointer(lease.playerTwo, facts::kControllerPawn);
    const void* active = ReadPointer(lease.playerTwo, kControllerActiveVehicle);
    return std::format(L"member controller {} (effective {}); {} controller {} (effective {}); PC2's pawn {}, active "
                       L"coordinator {}",
                       Who(ReadPointer(lease.member, facts::kPawnController)),
                       Who(ReadPointer(lease.member, facts::kCharacterEffectiveController)), word,
                       Who(ReadPointer(lease.vehicle, facts::kPawnController)),
                       Who(ReadPointer(lease.vehicle, facts::kCharacterEffectiveController)),
                       pawn == lease.vehicle ? std::wstring(L"the ") + word
                       : pawn == lease.member ? std::wstring(L"the member")
                       : pawn != nullptr      ? std::wstring(L"other")
                                              : std::wstring(L"none"),
                       active == lease.coordinator && active != nullptr ? L"player 2's own"
                       : active != nullptr                              ? L"other"
                                                                        : L"none");
}

bool GiveBack(const Lease& lease, SwitchAiFn switchAi, const wchar_t* label, const wchar_t* what) noexcept
{
    bool held = PlayerTwoHolds(lease.member, lease.playerTwo);
    if (!held && ReadPointer(lease.member, facts::kPawnController) != lease.playerTwo)
    {
        RestoreCall restore{lease.member, lease.playerTwo, switchAi};
        member_movement::InPrimaryScope(lease.member, &RunRestore, &restore);
        held = PlayerTwoHolds(lease.member, lease.playerTwo);
        platform::Log(L"{}: {} left the member to her AI; the game's possession call {}", label, what,
                      held ? L"gave her back to player 2" : L"DID NOT give her back");
    }
    return held;
}

void LogNotHeld(const wchar_t* label) noexcept
{
    platform::Log(L"{} STOPPED: player 2's controller does not hold the member after the ride; player 2 stays a "
                  L"passenger this session (the session's own checks take it from here)",
                  label);
}

VehicleTick TickVehicle(Lease& lease, void* state, const move_state::EngineTicks& engine,
                        std::uint64_t withoutKey) noexcept
{
    if (engine.count == 0 && !engine.rideTicked)
    {
        return VehicleTick::Waiting;
    }
    const float deltaTime = engine.rideTicked ? engine.rideDeltaTime : engine.deltaTime;
    const auto  result    = member_movement::TickRide(lease.playerTwo, lease.member, state, lease.component, deltaTime,
                                                      withoutKey);
    switch (result)
    {
        case member_movement::RideTick::Faulted: return VehicleTick::Faulted;
        case member_movement::RideTick::Ticked: ++lease.ticks; return VehicleTick::Ticked;
        case member_movement::RideTick::Refused: break;
    }
    ++lease.refused;
    return VehicleTick::Refused;
}

bool Riding() noexcept
{
    return chocobo_ride::Driving() || wheelie_ride::Riding();
}

void Release(const wchar_t* why) noexcept
{
    chocobo_ride::Release(why);
    wheelie_ride::Release(why);
}

Forgotten Forget() noexcept
{
    return {chocobo_ride::Forget(), wheelie_ride::Forget()};
}

}  // namespace lss::player_two::own_ride
