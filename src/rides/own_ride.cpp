#include "rides/own_ride.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "input/member_movement.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "rides/chocobo_ride.h"
#include "rides/wheelie_ride.h"

#include <format>

namespace lss::rides::own_ride {

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
    return camera::camera_access::ModuleAt(op, kModuleSlot15);
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

bool BoardingGateOpen(int seat, void* member, void* playerTwo, const wchar_t* label) noexcept
{
    const auto gate = input::member_movement::PlayerCoreAsSeat(seat, member, member);
    if (gate.answer == 1 && gate.primary == playerTwo)
    {
        return true;
    }
    platform::Log(L"{}: not boarded (the boarding's player-core test as {} reads {}, primary {}PC{})", label,
                  players::SeatName(seat), gate.answer, gate.primary == playerTwo ? L"" : L"not ", seat + 1);
    return false;
}

std::wstring PossessionText(const Lease& lease, const wchar_t* word)
{
    const void* pawn   = ReadPointer(lease.playerTwo, facts::kControllerPawn);
    const void* active = ReadPointer(lease.playerTwo, kControllerActiveVehicle);
    const bool own = active == lease.coordinator && active != nullptr;
    return std::format(L"member controller {} (effective {}); {} controller {} (effective {}); PC{}'s pawn {}, active "
                       L"coordinator {}{}",
                       players::Who(ReadPointer(lease.member, facts::kPawnController)),
                       players::Who(ReadPointer(lease.member, facts::kCharacterEffectiveController)), word,
                       players::Who(ReadPointer(lease.vehicle, facts::kPawnController)),
                       players::Who(ReadPointer(lease.vehicle, facts::kCharacterEffectiveController)), lease.seat + 1,
                       pawn == lease.vehicle ? std::wstring(L"the ") + word
                       : pawn == lease.member ? std::wstring(L"the member")
                       : pawn != nullptr      ? std::wstring(L"other")
                                              : std::wstring(L"none"),
                       own                 ? players::SeatName(lease.seat)
                       : active != nullptr ? L"other"
                                           : L"none",
                       own ? L"'s own" : L"");
}

bool GiveBack(const Lease& lease, SwitchAiFn switchAi, const wchar_t* label, const wchar_t* what) noexcept
{
    bool held = PlayerTwoHolds(lease.member, lease.playerTwo);
    if (!held && ReadPointer(lease.member, facts::kPawnController) != lease.playerTwo)
    {
        RestoreCall restore{lease.member, lease.playerTwo, switchAi};
        input::member_movement::InPrimaryScope(lease.seat, lease.member, &RunRestore, &restore);
        held = PlayerTwoHolds(lease.member, lease.playerTwo);
        if (held)
        {
            platform::Log(L"{}: {} left the member to her AI; the game's possession call gave her back to {}", label,
                          what, players::SeatName(lease.seat));
        }
        else
        {
            platform::Log(L"{}: {} left the member to her AI; the game's possession call DID NOT give her back", label,
                          what);
        }
    }
    return held;
}

void LogNotHeld(int seat, const wchar_t* label) noexcept
{
    platform::Log(L"{} STOPPED: {}'s controller does not hold the member after the ride; {} stays a passenger this "
                  L"session (the session's own checks take it from here)",
                  label, players::SeatName(seat), players::SeatName(seat));
}

VehicleTick TickVehicle(Lease& lease, void* state, const input::move_state::EngineTicks& engine,
                        std::uint64_t withoutKey) noexcept
{
    if (engine.count == 0 && !engine.rideTicked)
    {
        return VehicleTick::Waiting;
    }
    const float deltaTime = engine.rideTicked ? engine.rideDeltaTime : engine.deltaTime;
    const auto  result    = input::member_movement::TickRide(lease.seat, lease.playerTwo, lease.member, state, lease.component,
                                                      deltaTime, withoutKey);
    switch (result)
    {
        case input::member_movement::RideTick::Faulted: return VehicleTick::Faulted;
        case input::member_movement::RideTick::Ticked: ++lease.ticks; return VehicleTick::Ticked;
        case input::member_movement::RideTick::Refused: break;
    }
    ++lease.refused;
    return VehicleTick::Refused;
}

bool Riding(int seat) noexcept
{
    return chocobo_ride::Driving(seat) || wheelie_ride::Riding(seat);
}

void Release(int seat, const wchar_t* why) noexcept
{
    chocobo_ride::Release(seat, why);
    wheelie_ride::Release(seat, why);
}

Forgotten Forget(int seat) noexcept
{
    return {chocobo_ride::Forget(seat), wheelie_ride::Forget(seat)};
}

}  // namespace lss::rides::own_ride
