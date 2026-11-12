#pragma once

// Moving each seat's party member with its own pad (players/seats.h).
//
// The game's field tick runs one move state a frame, the party leader's, so the member a seat's player
// possesses never reads a stick. Once per frame for each seat, after the game's ticker has run, this
// ticks the member's move state with the game's own per-character tick, inside three scopes:
//   - the seat's pad as the merged stick (input/pad_sticks.h);
//   - the seat's player as the primary player, so the tick takes its direction from that player's camera
//     (scopes/primary_controller.h);
//   - a yes from the movement gate's party-leader test for the member's player type only
//     (scopes/leader_answer.h: its slot answers on the game thread, inside this tick, and nowhere else).
// It runs only while per-pad sticks are on. Afterwards the member's field-action rows (the party's
// summons, which the tick asks for from whichever state it runs) are cleared with the game's own
// handle clear, as for any character the game did not tick; otherwise they would show as extra dim
// rows in the one shared prompt list. Seats take turns: one seat's tick and its scopes end before the
// next seat's begin.
//
// Per seat: the counters and the stop after an engine exception, the battle target fields opened around the tick, and
// the rides' calls inside the tick (chocobo_ride, wheelie_ride, own_ride: each finds the seat from the controller). The
// dev watches inside it stay player 2's. Game thread only.

#include "players/seats.h"
#include "players/stage_counters.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::input::member_movement {

/// Finds the tick and replaces the leader-test slot. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Once per game tick while `seat`'s player plays `member` with `controller` (the seat's).
void Tick(int seat, void* controller, void* member) noexcept;

/// `seat`'s: acted: the member was ticked with every scope held; refused: not ticked because a scope
/// could not be held or a precondition failed; passed: not ticked because the game's ticker did not run
/// or ticked the member itself. If the engine raises an exception inside the tick, the seat's tick stops
/// for the rest of the session (logged).
players::StageCounters& Counters(int seat) noexcept;

/// The boarding gate of a seat's rides (chocobo_ride, wheelie_ride; research/RE-p2-chocobo-ride.md): the game's
/// IsUCPC_Core(`character`) as the seat's chocobo swap would see it, inside the seat's primary-controller scope and its
/// member's leader answer, and the primary controller seen there. Calls nothing else. -1 when a scope could not be
/// held or the test is unavailable. Game thread.
struct CoreAsSeat
{
    std::int32_t answer  = -1;
    const void*  primary = nullptr;
};
CoreAsSeat PlayerCoreAsSeat(int seat, const void* character, const void* member) noexcept;

/// The game's `test(character)` (IsUCPC_Core) with the leader test's yes for a seat's ridden chocobo's input
/// component held off, so the chocobo answers as player 1's ridden chocobo does (no). The member's type answer stays.
/// For the vault's camera check (camera/vault_camera). Game thread.
std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept;

/// Runs `call(context)` inside `seat`'s primary-controller scope and the member's leader answer (rides/chocobo_ride
/// runs the game's ride swap there). False, calling nothing, when the scope could not be held. Game thread.
bool InPrimaryScope(int seat, const void* member, void (*call)(void* context), void* context) noexcept;

/// The same with the seat's pad's buttons too, for a game call that also reads the merged pads' held keys (a chocobo
/// launch pad's Accel). False, calling nothing, when a scope could not be held. Game thread.
bool InPrimaryScopeWithButtons(int seat, const void* member, void (*call)(void* context), void* context) noexcept;

/// Removes the rows of a seat's ridden chocobo's own prompt handles with the game's handle clear (as the chocobo
/// state's own deactivate does), at the end of the seat's ride. Game thread.
void ClearRideRows(void* chocoboState) noexcept;

/// Ticks a seat's ridden chocobo's move state with the game's per-character tick, inside every scope of the
/// member's tick (the seat's pad, primary, buttons, the mod's own tick, the scope-leak put-backs), the leader answer
/// also saying yes for `component` (the chocobo's input component), and the seat's buttons without `withoutKey` (an
/// FName; 0 for none). Game thread.
enum class RideTick
{
    Ticked,
    Refused,  // a scope could not be held
    Faulted,  // the engine raised an exception inside the tick
};
RideTick TickRide(int seat, void* controller, void* member, void* state, const void* component, float deltaTime,
                  std::uint64_t withoutKey) noexcept;

/// Runs `engine`, the game's slot 1 of a seat's ridden chocobo's move state (called by the chocobo's own actor tick:
/// its handles and the anim sync), inside the seat's primary-controller scope and the leader answer for the member's
/// type and `component`: the context the game gives the chocobo player 1 rides (research/RE-p2-chocobo-run-refresh.md).
/// Refused calls nothing (the caller runs `engine` as the game would). With `sample`, `ridden` gets the game's
/// ridden-vehicle test (IsActiveRiddenVehicle) for `chocobo` outside and inside the scope (-1: not evaluated).
/// Game thread.
using HandlesFn = std::uintptr_t (*)(void* state, float deltaTime);
struct HandlesTick
{
    RideTick       result        = RideTick::Refused;
    std::uintptr_t returned      = 0;  // the engine's rax, passed through
    std::int32_t   riddenOutside = -1;
    std::int32_t   riddenInside  = -1;
};
HandlesTick TickRideHandles(int seat, const void* member, void* state, const void* chocobo, const void* component,
                            float deltaTime, HandlesFn engine, bool sample) noexcept;

}  // namespace lss::input::member_movement
