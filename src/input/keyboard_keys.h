#pragma once

// The keyboard as one more device in the player windows' input (session/player_window), as input/pad_table makes each
// pad one (research/RE-kbm-square-input.md).
//
// Slate hands every key to the game's input preprocessors before its focus path and the viewport, Square's first; a
// true answer from Square's key handlers ends the key there. The mod takes Square's key-down and key-up slots:
//   - Tab, the opener (the game's map key, as View is the pad's map button): its press is held back from the game. Held
//     a second, it is the keyboard's opener, told to the listener as a pad's View hold is; let go sooner, the game gets
//     the press then (through the game's own key-to-actions pass, as its key repeat presses a key) and its release a
//     frame later, so a tap still opens the map, a tap's length late (a press and release in one frame read as
//     neither);
//   - the period, while the keyboard plays player 2 or 3: held back from the game, it asks to bring that player back to
//     player 1, as a pad's L3 twice (world/nav_strand, TakeWarpPress);
//   - while the keyboard's player's window is open (SetIdle), every key goes to the window and none to the game, as a
//     pad reads idle then: the keys that stand for the pad's (pad_rules::FromKey) are told to the listener as that
//     pad's presses, Backspace as B's tap or hold. A key held from before the window opened is let go to the game as
//     it came.
// The keyboard is pad_rules::kKeyboardPad in the listener's events. Key releases also note their Slate user for the
// battle menu's release route (ReleaseUser).
// The game's key, mouse-button and wheel handlers also mark the keyboard as the last device used, which turns every
// view's button art to keycaps and player 1's rumble to the keyboard. While the keyboard plays player 2 or 3 and views
// are split, that mark is put back after each of its events (the mouse's two handlers are taken for it alone):
// player 1's device keeps the game's art, and the keyboard player's view gets its keycaps from hud/button_art. With one
// view on screen the keyboard marks itself as any device does.

#include "input/pad_table.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::input::keyboard_keys {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The key slots are the mod's.
bool Installed() noexcept;

/// The player windows' input from the keyboard (the same events as the pads', pad_table::Listen). Game thread.
void Listen(pad_table::Listener listener) noexcept;

/// Once per tick: Tab held a second opens, Backspace's tap or hold. Game thread.
void Poll() noexcept;

/// The keyboard's player's window is open: every key is the window's. Game thread.
void SetIdle(bool idle) noexcept;

/// The seat the keyboard plays (1 or 2: player 2 or 3) if the period was pressed since the last take, else -1; taken
/// once a tick, so a press made while the keyboard plays player 1 is dropped. Game thread.
int TakeWarpPress() noexcept;

/// ReleaseUser's answer for a keyboard key's release (player 1's pad shares the keyboard's Slate user, 0).
inline constexpr std::int32_t kKeyboardRelease = -2;

/// Who sent the key release being handled now: a pad's controller id, kKeyboardRelease for a key, or -1 outside one (a
/// mouse button's release, the input tick's own). Game thread.
std::int32_t ReleaseUser() noexcept;

#if LSS_DEV
/// The `players.keyboard` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::input::keyboard_keys
