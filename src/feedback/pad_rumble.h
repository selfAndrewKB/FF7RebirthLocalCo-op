#pragma once

// Each player's rumble on their own pad (RE-p2-rumble.md §6.1, §9; plan, "per-pad rumble").
//
// Force feedback is per controller in the engine: each PC sends its own values with its own ControllerId. Two things
// stop a seat's pad from getting its rumble (both seen live): the seat's send ran before PC1's in each frame, and PC1's
// send (ControllerId 0, turned by Square's platform layer into the "last active device" code) writes every pad, so the
// seat's pad got PC1's write last; and an input from it made it player 1's last active device, so player 1's rumble
// moved to it.
// Both levers are the game's own:
//   - A (stock UE4): once per pair of controllers, before the world tick, the seat's PC calls
//     AddTickPrerequisiteActor(PC1) through ProcessEvent: its actor tick, and so its send, comes after PC1's;
//   - D (Square's input layer): while a seat is in, an input event from the seat's pad does not call
//     SquareInput__NoteLastActiveDevice, so player 1's last device (rumble, button glyphs, disconnect check) stays
//     player 1's.
// Logged: the ordering call and the controller's prerequisite count, the device code on change, skips per minute.
// Game thread.
//
// Per seat (players/seats.h): each seat's controller is ordered after player 1's (its own pair, once), and every
// seat's pad is kept out of player 1's last device (a pad per seat; the skips counted per seat).

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::pad_rumble {

/// Finds the UFunction and installs the last-device redirect (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out).
void Remove() noexcept;

/// Once per frame before the world tick (the engine-tick callback): orders each seat's new pair of controllers and
/// follows each seat's pad.
void BeforeTick() noexcept;

}  // namespace lss::feedback::pad_rumble
