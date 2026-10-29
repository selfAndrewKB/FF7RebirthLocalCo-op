#pragma once

// The buggy and the Tiny Bronco are each one vehicle for the whole party: while player 1 rides one, the players take
// turns at it (the user, 2026-10-02 for the buggy: "split screen is disabled for that anyway, because two mirror images
// would be weird"; 2026-10-04: the same rule for the bronco, player 2 sits out the crossing; plan, "Buggy: single view"
// and "Airboat").
//
// The signal is the game's own: player 1's AEndPlayerController::ActiveVehicleCoordinator (`PC+0x5B8`) is the buggy's
// or the bronco's coordinator (by its vtable) in a state other than idle (0):
//   - the buggy's state id `*[+0x1E8]`: 1 before boarding, 3 boarding, 4 riding, 5 getting off, then `0x5B8` is null
//     (RE-p2-control-mode-object.md, confirmed live 2026-10-04);
//   - the bronco's `*[+0x70]`: 2 registered (0-1 tick), 3 riding, 4 getting off (one tick, or none when immediate); an
//     end request in state 2 leaves `0x5B8` naming an idle (0) coordinator, which must not hold player 2 aside
//     (RE-p2-tiny-bronco.md).
// While it holds, the caller (mod.cpp):
//   - has player 2 stand aside (Session::SetSharedRide): player 2's member goes back to its AI, as in the one-player
//     game, and player 2 stays in without one; the 2nd slot's member is taken again once the ride is over;
//   - shows player 1's single view;
//   - stands the per-pad input down (input::pad_sticks::SetStoodDown), so the game's own merge reaches player 1 from every
//     pad, as in the one-player game.
// Logged (Info): each ride's start and end (and whether the end passed through getting off), and at each vehicle state
// where the member player 2 gave back is (metres from the vehicle, hidden, its controller, its AI state), the vehicle's
// controller, the party manager's idle byte and whether a battle runs; a battle starting or ending during a ride. Game
// thread only.
//
// Per seat (players/seats.h): the ride is one fact, player 1's vehicle, read while any seat is in; every seat in stands
// aside together. Its consumers loop the seats (the session's stand-aside, each seat's rumble copy in mod.cpp). The
// members given back are logged per seat.

namespace lss::platform {
class GameImage;
}

namespace lss::rides::shared_ride {

/// Finds the coordinators' vtables; logged either way. False if none was found (no shared ride is then seen).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, before the session's: whether player 1 rides a shared vehicle now (false without player 2, or
/// when not installed).
bool Tick() noexcept;

/// Game thread: player 1's controller rides the buggy or the bronco now (the test Tick makes, without player 2: a join
/// while player 1 rides lets player 2 in standing aside). Read-only.
bool Rides(const void* playerOneController) noexcept;

}  // namespace lss::rides::shared_ride
