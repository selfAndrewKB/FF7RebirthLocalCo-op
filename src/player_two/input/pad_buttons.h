#pragma once

// Each pad's buttons for its own player (RE-p2-per-pad-buttons.md).
//
// Square's input object (the one that holds the sticks, player_two/input/pad_sticks.h) keeps every
// gameplay action in a table at +0xF8, with a UI-filtered twin at +0x148. Each 40-byte element is
// an FName key and, at +0x18, the merged state: previous-held, held, repeat, a changed-this-frame
// latch, and a 32-bit mask of the sources holding it (confirmed live: pad 1 sets bit 0, pad 2 bit
// 1, the keyboard bit 17). The game reads "pressed" as previous == 0 and held == 1, so its edges
// come from the merged state. The key handlers write the tables while the devices are polled, and
// the input processor's Tick advances previous = held once per frame, all before the world ticks.
//
// With per-pad input on (player_two/input/pad_sticks.h) and player 2 in:
//   - after the game's Tick (its slot in the IInputProcessor vtable is replaced), every element's
//     previous/held bytes are rewritten to player 1's view: pad 1, keyboard and the game's own
//     synthetic keys, never pad 2 (player_two/input/button_view.h). Everything that reads buttons outside
//     player 2's calls (player 1's input, the leader's move state, the menus, battle) sees player 1.
//   - pad 2's action-downs reach the tables (for player 2's polling) but are passed to the game's
//     dispatcher as already handled, its own flag, so pad 2 never opens a menu or runs a field prompt
//     (or they go to player 2's own UI through the route below). This covers the XInput main loop,
//     two DirectInput loops and, from the first input Tick, the XInput extra-key calls (the shortcut
//     keys), whose call sites cross a code block and are changed from the game thread only;
//   - a ButtonScope, opened around player 2's calls (their controller input and their member's
//     tick, next to the stick scope), writes player 2's view and puts player 1's back after.
// The repeat, latch and source bytes are never written. The game's own writers keep working on
// top: its press update (previous = held, held = 1) and release (when the mask empties) produce the
// same bytes a one-pad game would hold, and the next Tick rewrites both views.
//
// Changes to held state or sources are also logged while player 2 is in (capped per status line).

#include "platform/dev_switch.h"

#include <cstddef>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::pad_buttons {

/// Replaces the Tick slot after checking it. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Player 2's view of the buttons for the length of one engine call. Game thread, player 2 in and
/// per-pad input on; does not nest. `withoutKey` (an FName, 0 for none): an action player 2's view
/// shows as not held for this call (player 2's ride keeps the getoff button out: player 1 owns mounting).
class ButtonScope
{
public:
    explicit ButtonScope(std::uint64_t withoutKey = 0) noexcept;
    ~ButtonScope();
    ButtonScope(const ButtonScope&)            = delete;
    ButtonScope& operator=(const ButtonScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    bool held_ = false;
};

/// The game's action-down dispatcher: (owner, unused, source bit, the action event, alreadyHandled),
/// 1 when the UI consumed the action.
using ActionDownFn = char (*)(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled);

/// A route for pad 2's presses to player 2's own UI (player_two/battle/command_menu_tick.h): true when it
/// took the press (its dispatch result in `result`); false leaves the press kept from the menus.
using PlayerTwoRouteFn = bool (*)(ActionDownFn dispatch, void* owner, void* unused, std::uint32_t sourceBit, void* event,
                                  std::uint8_t alreadyHandled, char& result);
/// Which player's pad drives the menus (0 player 1, the default; 1 player 2, while a shop player 2 opened is up,
/// player_two/vending): that player's view is the one the tables show outside player 2's calls, and the other pad's
/// presses are passed to the game's dispatcher as handled. Stick navigation (the game's synthetic d-pad from the merged
/// stick) is not split. Logged on change. Game thread.
void SetMenuPlayer(int player) noexcept;

/// Sets the route (null: none). Game thread.
void SetPlayerTwoRoute(PlayerTwoRouteFn route) noexcept;

/// Diagnosis: which parts act (bits: 1 the split views, player 1's after the Tick and player 2's in
/// their calls; 2 pad 2's presses kept from menus and prompts). Both at start. Logged.
inline constexpr unsigned kPartViews  = 1;
inline constexpr unsigned kPartEvents = 2;
void SetMask(unsigned mask) noexcept;
unsigned Mask() noexcept;

/// The status line: views written, scopes held, changes logged and dropped since the last call.
void LogStatus() noexcept;

#if LSS_DEV
/// The `buttons-mask` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::player_two::pad_buttons
