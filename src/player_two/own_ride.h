#pragma once

// The lease layer the two own rides share (player_two/chocobo_ride.h, player_two/wheelie_ride.h): player 2's
// controller holds a vehicle through player 2's own coordinator, the member is her AI's for the ride, and at the end
// player 2's controller must hold the member again. The parts both rides need are here once: the install lookups, a
// coordinator's state, the ride camera module, the vehicle's input component, "player 2 holds it", the boarding's
// player-core gate, the lease's possession text, the give-back after the game's end (the game's possession call when
// the end left the member to her AI), one frame of the vehicle's tick inside player 2's scopes, and the session's
// view of both rides. Each ride keeps its own state machine: the chocobo waits for the game's getoff and the end of
// its coordinator, the Wheelie has a swap back, a detach notify and the game's forced getoffs. Game thread only.

#include "player_two/move_state.h"

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
struct FunctionFact;
}

namespace lss::player_two::own_ride {

/// A game function's live address, or 0 (the ride checks every one and refuses to install on a 0).
std::uintptr_t FindOrZero(const platform::GameImage& image, const platform::FunctionFact& fact) noexcept;
std::uintptr_t RebaseOrZero(const platform::GameImage& image, std::uintptr_t ida) noexcept;

/// A coordinator's state id: its current state row (`rowField` -> u32 id) when its vtable is `coordVt`; -1 otherwise.
std::int32_t CoordState(const void* coordinator, std::uintptr_t coordVt, std::uint32_t rowField) noexcept;

/// Player 2's camera's slot-15 module (the ride framing), or null.
const void* CameraModule15(const void* playerTwo) noexcept;

/// `vehicle`'s input component: its control holder's getter (+0x638 -> vt+0x40), or null.
const void* InputComponentOf(const void* vehicle) noexcept;

/// Player 2's controller holds `character`: its effective controller, and the controller's pawn.
bool PlayerTwoHolds(const void* character, const void* playerTwo) noexcept;

/// The boarding's possession swap runs the game's player-core test on the member: as player 2 it must pass. False,
/// logged under `label`, when it does not (the ride is not taken).
bool BoardingGateOpen(void* member, void* playerTwo, const wchar_t* label) noexcept;

/// The lease every ride records; each ride adds its own fields.
struct Lease
{
    void*         playerTwo   = nullptr;
    void*         member      = nullptr;
    void*         vehicle     = nullptr;  // the chocobo or the scooter
    void*         coordinator = nullptr;  // player 2's own coordinator for it
    std::uint64_t weak        = 0;        // the vehicle, as the game's weak pointer names it
    const void*   component   = nullptr;  // its input component (the leader answer's second yes)
    std::uint32_t ticks       = 0;        // the vehicle's ticks in player 2's scopes
    std::uint32_t refused     = 0;        // ... a scope could not be held
};

/// The possession fields for the log: "member controller X (effective Y); <word> controller X (effective Y); PC2's
/// pawn <the word / the member / other / none>, active coordinator <player 2's own / other / none>".
std::wstring PossessionText(const Lease& lease, const wchar_t* word);

using SwitchAiFn = void (*)(void* character, std::uint8_t toPlayer, void* controller);

/// After the game's end of the ride: player 2's controller must hold the member again. If the end left her to her AI,
/// the game's possession call inside the primary scope (logged under `label`; `what` names the end: "the end", "the
/// getoff"). Returns whether player 2 holds her now.
bool GiveBack(const Lease& lease, SwitchAiFn switchAi, const wchar_t* label, const wchar_t* what) noexcept;

/// The STOPPED line when GiveBack returned false: player 2 stays a passenger this session.
void LogNotHeld(const wchar_t* label) noexcept;

enum class VehicleTick
{
    Waiting,  // the field ticker is idle (a menu, an event): the vehicle waits too
    Ticked,
    Refused,  // a scope could not be held
    Faulted,  // the engine raised an exception inside the tick
};

/// One frame of the vehicle's move state ticked with the game's per-character tick inside player 2's scopes, on the
/// frames the field ticker runs (member_movement::TickRide; `withoutKey`: a button kept out of pad 2's view, 0 none).
/// Counts in the lease.
VehicleTick TickVehicle(Lease& lease, void* state, const move_state::EngineTicks& engine,
                        std::uint64_t withoutKey) noexcept;

// ---- Both rides, for the session ----

/// Player 2 drives their chocobo or rides a Wheelie.
bool Riding() noexcept;

/// Before the session gives the member back, withdraws player 2 or leaves: both rides' swap back, while player 2's
/// scopes still hold. Logged by the ride.
void Release(const wchar_t* why) noexcept;

/// The member was destroyed under player 2: both rides forgotten without calls. The vehicles player 2's controller
/// possessed (null if none), so the session clears that controller field too.
struct Forgotten
{
    const void* chocobo = nullptr;
    const void* scooter = nullptr;
};
Forgotten Forget() noexcept;

}  // namespace lss::player_two::own_ride
