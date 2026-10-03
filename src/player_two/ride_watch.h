#pragma once

// Logs, read-only, the game's chocobo and buggy rides while player 2 is in (RE-p2-chocobo-ride.md §5; plan §3,
// "Player 2 rides their own chocobo" and "Buggy: single view"). Logging first: nothing here changes the ride.
//
// Every 5 ticks: player 1's active vehicle coordinator (AEndPlayerController::ActiveVehicleCoordinator, `PC+0x5B8`;
// its kind by vtable and its state), player 2's (expected empty), the chocobo ride manager's state and player 2's
// member's entry in it, player 2's member's chocobo (ridden flag, move state, controllers, AI state, the input
// component the gate's player-core test reads and that test's answer), and player 2's member (attach parent,
// controllers). Logged on each change (the AI state and the player-core answers, which may flicker, are left to
// the snapshot); while a ride or vehicle is active, also a snapshot every second with those, the merged sticks
// and player 1's ride speed (the RE's test 3: does pad 2 steer player 1's chocobo?). Calls only the game's
// weak-pointer resolve and assign and its player-core test, guarded; the test reaches the mod's own leader slot
// (member_movement), which answers as the game outside player 2's scope, so it changes nothing there. The one
// exception to "read-only": once a second while riding, the dry run of the chocobo swap's gate
// (member_movement::PlayerCoreAsPlayerTwo) opens the mod's primary-controller scope for one guarded test call, as
// the member's tick does each frame; if the game moved the primary controller inside that call, the scope's close
// faults the generation as everywhere else. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::ride_watch {

/// Finds the globals and the test; logged either way. False if a check failed (nothing is sampled then).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick.
void Tick() noexcept;

}  // namespace lss::player_two::ride_watch
