#pragma once

// Each view's button prompts in its own player's art: keycaps, Xbox or PlayStation (research/RE-per-player-icons.md).
//
// Nearly every button icon the game draws is text: a `<button=NAME>` tag, turned into art when its text block is laid
// out, from two bytes of the block's font (hud/button_art_rules). The game keeps one answer for everyone: its three UI
// fonts carry the art of the last device used, written on each device change (player 1's here: the other players'
// pads are kept out of that note while views are split, feedback/pad_rumble, and so is the keyboard's flag while it
// plays player 2 or 3, input/keyboard_keys). Player 1's view therefore shows the game's own art, and each other
// player's HUD pass shows theirs:
//   - before the pass (the seat's levers and HUD updates follow), when the seat's art differs from the game's, every
//     live text block that holds a button tag gets the seat's own laid-out state swapped in (hud/label_states: player
//     1's set aside), and the three UI fonts get the seat's two bytes;
//   - a block whose seat state is new, or was laid out for another text, art or font, starts from an empty state with a
//     relayout pending, which the block's own Tick runs when the pass paints it (with the seat's bytes in the fonts):
//     the art is the game's own relayout, made once per change, never per frame;
//   - after the pass, the fonts get the game's bytes back and player 1's states go back in; a button block that was laid
//     out inside the pass without a seat state of its own (new, or one whose text gained a button) is laid out again
//     for player 1 with the game's bytes.
// While the screen shows one view (menus, minigames, scenes) no pass runs: every view is the game's, and every
// player's device moves the game's art as in single player.
// Logged: each player's art on change (Info), a census of the button blocks and the passes' counts (Debug). Game
// thread.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::button_art {

bool Install(const platform::GameImage& image) noexcept;

/// Player 1's state back in every block, every record freed.
void Remove() noexcept;

/// Before a further column's HUD pass, for the seat it shows (0: player 1's content, a mirror or a copy: nothing).
void EnterPass(int seat) noexcept;

/// After that pass's render, before player 1's HUD updates run again.
void LeavePass() noexcept;

/// Once per engine tick, outside the passes: the records of dead blocks dropped, a seat that left released, the
/// census lines.
void Tick() noexcept;

#if LSS_DEV
/// The `button-art` switch and the `players.button-art` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::hud::button_art
