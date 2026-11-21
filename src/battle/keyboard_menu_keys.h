#pragma once

// The keyboard's movement keys follow the command menu of the player the keyboard plays (research/RE-kbm-square-input.md,
// the movement keys as the d-pad).
//
// While the command menu is open, the game turns the keyboard's movement keys into the d-pad, so they work the menu's
// cursor and stop moving the character: the UI system's tick asks the command controller's current state each frame and
// sets one bit of Square's input object while the menu is open (clearing it after), and the key-down reads the movement
// keys as the d-pad while that bit is set, letting go of the ones held when it comes on. The input object is one for
// every player and the tick asks player 1's controller, so player 1's open menu took the keyboard's movement from the
// player 2 or 3 it plays.
//
// The tick's call of the state read is taken: while the keyboard plays player 2 or 3, it answers that player's own
// controller's state (no menu while theirs is not built), so the game's bit follows the keyboard's own player's menu;
// otherwise it answers as the game's. Logged: the install at Info; at Debug each change of the keyboard player's menu,
// with player 1's beside it. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::keyboard_menu_keys {

/// Takes the tick's call of the state read (checked; logged). False if the check fails: the keyboard's movement keys
/// then follow player 1's menu, as the game's.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::keyboard_menu_keys
