#pragma once

// Each seat's own force-feedback manager (research/RE-p2-ffb-manager-per-pc.md).
//
// Each controller embeds a force-feedback component (`PC+0x758`) that plays on its owner's pad, but every component
// names the same manager: a lazy singleton whose 16 tracks remember the effect each channel plays now. The game's apply
// skips a play when the track already names the effect, so the same effect for two players at once (a shared ride's
// copy, two players on chocobos) played on one pad only.
//
// A seat's component gets a manager of its own, made the way the game's getter makes one (its allocator and
// constructor), and made where the game makes its own: inside a player controller's constructor. The manager's
// constructor finds its effects with FObjectFinder, which only a constructor may use (called from the engine tick, it
// ends the game with a fatal error), so the component constructor's call in the AEndPlayerController
// constructor is redirected: for the controller a join's CreatePlayer constructs (a seat's), the manager is made there
// the first time, its 16 tracks built at once by the game's builder (names only; a play writes a track without a bounds
// check), and the component names it before BeginPlay. BeginPlay then registers the component there and sets its
// Vibration flag, as it does for player 1's. The game's state 1 (the resident sounds, the ready flag) runs on the next
// engine tick, outside every constructor, as the game's own runs at map loads.
//
// The Vibration option (the setter on the global) and the stop-all (fades, menus, state changes; the global, on the
// primary controller) are repeated on the seat's manager, the stop-all with the seat as the primary player. At leave the
// component goes back to the global manager (the game's Contains, grow and remove, as BeginPlay and the component's
// destructor keep the registries). The seat's manager is kept for the session (the game never frees its own either) and
// reused at the next join.
//
// Game thread throughout.
//
// Per seat (players/seats.h): each seat's manager, its component's attachment and its logged states are its own (by
// seat, index 0 unused); the Vibration option is repeated on every seat's manager and the stop-all on each attached
// seat's, the seats in turn. The component constructor takes its seat from the join (session/own_controller.h).

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::seat_ffb_managers {

/// Finds the game's functions and replaces the option setter's and the stop-all's entries (all or none; logged).
void Install(const platform::GameImage& image) noexcept;

/// Puts every seat's component back on the global manager (game thread) and restores the entries.
void Remove() noexcept;

/// Once per engine tick: for each built seat, gives its component its manager once its controller has begun play.
void Tick() noexcept;

/// `seat` leaves (while its controller is still the generation's): its component goes back to the global.
void SeatOut(int seat) noexcept;

/// `seat`'s component names the seat's manager now (game thread).
bool Attached(int seat) noexcept;

}  // namespace lss::feedback::seat_ffb_managers
