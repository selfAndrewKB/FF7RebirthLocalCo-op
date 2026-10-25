#pragma once

// Player 2 rides their own chocobo (plan §3, "Player 2 rides their own chocobo"; RE-p2-chocobo-ride.md §7, Lever 1).
//
// When player 1 mounts, the game mounts the whole party: player 1's chocobo is boarded through player 1's own
// chocobo coordinator (possessed by player 1's controller, the ride camera module on player 1's camera, the ride
// state that lets it run and dash); the others' chocobos appear by them, they are seated, and the chocobos follow
// player 1 under their own AI. The game never boards a member. On the first tick the game has seated player 2's
// member, this gives player 2's controller its own chocobo coordinator with the game's creator and runs the game's
// own boarding on it for her, instant (RE-p2-chocobo-ride.md §9, Lever 2): player 2's controller possesses the
// chocobo, the chocobo camera module goes on player 2's camera, and the ride status the gait reads is player 2's.
// Each frame the game's field ticker runs player 1's ride; player 2's chocobo's move state is ticked with the game's
// own per-character tick inside player 2's scopes (player_two/input/member_movement.h; player 2's controller's input
// pass
// ticks the coordinator). Pad 2's getoff button is kept out (player 1 owns mounting: it would dismount the party).
// When the game dismounts the party, player 2 waits for the game's getoff animation to detach her, then the game's
// own end of player 2's coordinator gives the member back; a battle, a give-back, a leave, the member detached or
// the chocobo gone end it at once.
//
// The mod's parts are the triggers, the tick of a body the field ticker does not select, and the getoff key kept
// out of pad 2's view; every call is the game's own. Everything is logged. Game thread only.
//
// Per seat (players/seats.h): each seat's ride, its held-back chocobo and its "stopped for the session" are its own
// (a record per seat, index 0 unused); the seat comes from the controller the member's tick hands in, or from the
// chocobo or move state the engine hands a wrapper. The install's lookups and the reflection offset are shared.

#include "player_two/input/move_state.h"
#include "players/seats.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::chocobo_ride {

/// Finds the game's functions; logged either way. False if a check failed (player 2 stays a passenger then).
bool Install(const platform::GameImage& image) noexcept;

/// From the member's tick while a seat's player plays `member` with `playerTwoController` (the seat's controller: the
/// seat is found from it), with the game's move-state ticks this frame. True while the seat's player drives their
/// chocobo: the member's own tick is skipped then.
bool Tick(void* playerTwoController, void* member, const move_state::EngineTicks& engine) noexcept;

/// `seat`'s player drives their chocobo now.
bool Driving(int seat) noexcept;

/// The chocobo `seat`'s player rides (riding or getting off, and still alive), or null. Game thread.
const void* DrivenChocobo(int seat) noexcept;

/// The chocobo `seat`'s player rides while seated (not getting off; still alive), or null. Game thread.
const void* RiddenChocobo(int seat) noexcept;

/// The move state of the chocobo `seat`'s player drives (its prompt handles are the seat's rows;
/// hud/field/prompt_rows.h), or null when not driving or it no longer checks out. Game thread.
const void* DrivenMoveState(int seat) noexcept;

/// The built seat whose player rides `chocobo` seated (RiddenChocobo), or drives it (DrivenChocobo), or -1.
int SeatRiding(const void* chocobo) noexcept;
int SeatDriving(const void* chocobo) noexcept;

/// When `chocobo` is the chocobo a seat's player rides (seated), runs `call` inside that seat's primary scope and
/// leader answer, as the game runs the chocobo player 1 rides, and returns true. False, calling nothing, for any other
/// chocobo or when a scope is refused. Game thread.
bool RunAsRider(const void* chocobo, void (*call)(void* context), void* context) noexcept;

/// From player_two/move_state's wrapper of the chocobo move state's slot 1 (every chocobo's, run by its own actor tick):
/// when `state` is the move state of the chocobo a seat's player rides, runs `engine` (the game's slot 1) inside its
/// primary scope and leader answer, as the game runs the chocobo player 1 rides, so the game's anim sync leaves the
/// gait, speed and run timer to the seat's input (RE-p2-chocobo-run-refresh.md §4, lever 1). False, calling nothing,
/// for any other state or when a scope is refused: the caller runs `engine` as the game would. `returned` gets the
/// engine's rax. Logged. Game thread.
bool TickHandles(void* state, float deltaTime, std::uintptr_t (*engine)(void* state, float deltaTime),
                 std::uintptr_t& returned) noexcept;

/// Before the session gives `seat`'s member back, withdraws the seat or it leaves: the swap back, while the seat's
/// scopes still hold. Logged. Does nothing when the seat's player is not driving.
void Release(int seat, const wchar_t* why) noexcept;

/// The member was destroyed under `seat`'s player: the ride is forgotten without calls. Returns the chocobo the seat's
/// controller possessed (null if not driving), so the session clears that controller field too.
void* Forget(int seat) noexcept;

}  // namespace lss::player_two::chocobo_ride
