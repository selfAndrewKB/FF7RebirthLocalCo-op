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

#include "player_two/input/move_state.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::chocobo_ride {

/// Finds the game's functions; logged either way. False if a check failed (player 2 stays a passenger then).
bool Install(const platform::GameImage& image) noexcept;

/// From the member's tick while player 2 plays `member`, with the game's move-state ticks this frame. True while
/// player 2 drives their chocobo: the member's own tick is skipped then.
bool Tick(void* playerTwoController, void* member, const move_state::EngineTicks& engine) noexcept;

/// Player 2 drives their chocobo now.
bool Driving() noexcept;

/// The chocobo player 2 rides (riding or getting off, and still alive), or null. Game thread.
const void* DrivenChocobo() noexcept;

/// The chocobo player 2 rides while seated (not getting off; still alive), or null. Game thread.
const void* RiddenChocobo() noexcept;

/// The move state of the chocobo player 2 drives (its prompt handles are player 2's rows; hud/field/prompt_rows.h), or
/// null when not driving or it no longer checks out. Game thread.
const void* DrivenMoveState() noexcept;

/// When `chocobo` is the chocobo player 2 rides (seated), runs `call` inside player 2's primary scope and leader answer,
/// as the game runs the chocobo player 1 rides, and returns true. False, calling nothing, for any other chocobo or
/// when a scope is refused. Game thread.
bool RunAsRider(const void* chocobo, void (*call)(void* context), void* context) noexcept;

/// From player_two/move_state's wrapper of the chocobo move state's slot 1 (every chocobo's, run by its own actor tick):
/// when `state` is the move state of the chocobo player 2 rides, runs `engine` (the game's slot 1) inside player 2's
/// primary scope and leader answer, as the game runs the chocobo player 1 rides, so the game's anim sync leaves the
/// gait, speed and run timer to player 2's input (RE-p2-chocobo-run-refresh.md §4, lever 1). False, calling nothing,
/// for any other state or when a scope is refused: the caller runs `engine` as the game would. `returned` gets the
/// engine's rax. Logged. Game thread.
bool TickHandles(void* state, float deltaTime, std::uintptr_t (*engine)(void* state, float deltaTime),
                 std::uintptr_t& returned) noexcept;

/// Before the session gives the member back, withdraws player 2 or leaves: the swap back, while player 2's scopes
/// still hold. Logged. Does nothing when player 2 is not driving.
void Release(const wchar_t* why) noexcept;

/// The member was destroyed under player 2: the ride is forgotten without calls. Returns the chocobo player 2's
/// controller possessed (null if not driving), so the session clears that controller field too.
void* Forget() noexcept;

}  // namespace lss::player_two::chocobo_ride
