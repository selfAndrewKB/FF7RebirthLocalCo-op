#pragma once

// Each player's rumble on their own pad (RE-p2-rumble.md §6.1, §9; plan, "per-pad rumble").
//
// Force feedback is per controller in the engine: each PC sends its own values with its own ControllerId. Two things
// stop player 2's pad from getting player 2's (both seen live): PC2's send ran before PC1's in each frame, and PC1's send
// (ControllerId 0, turned by Square's platform layer into the "last active device" code) writes every pad, so pad 2 got
// PC1's write last; and a pad-2 input made pad 2 player 1's last active device, so player 1's rumble moved to pad 2.
// Both levers are the game's own:
//   - A (stock UE4): once per pair of controllers, before the world tick, PC2->AddTickPrerequisiteActor(PC1) through
//     ProcessEvent: PC2's actor tick, and so its send, comes after PC1's;
//   - D (Square's input layer): while player 2 is in, an input event from player 2's pad does not call
//     SquareInput__NoteLastActiveDevice, so player 1's last device (rumble, button glyphs, disconnect check) stays
//     player 1's.
// Logged: the ordering call and the controller's prerequisite count, the device code on change, skips per minute.
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::pad_rumble {

/// Finds the UFunction and installs the last-device redirect (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with player 2 out).
void Remove() noexcept;

/// Once per frame before the world tick (the engine-tick callback): orders a new pair of controllers and follows
/// player 2's pad.
void BeforeTick() noexcept;

}  // namespace lss::player_two::pad_rumble
