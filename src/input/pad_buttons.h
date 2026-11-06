#pragma once

// Each pad's buttons for its own player (RE-p2-per-pad-buttons.md).
//
// Square's input object (the one that holds the sticks, input/pad_sticks.h) keeps every
// gameplay action in a table at +0xF8, with a UI-filtered twin at +0x148. Each 40-byte element is
// an FName key and, at +0x18, the merged state: previous-held, held, repeat, a changed-this-frame
// latch, and a 32-bit mask of the sources holding it (confirmed live: pad 1 sets bit 0, pad 2 bit
// 1, the keyboard bit 17). The game reads "pressed" as previous == 0 and held == 1, so its edges
// come from the merged state. The key handlers write the tables while the devices are polled, and
// the input processor's Tick advances previous = held once per frame, all before the world ticks.
//
// With per-pad input on (input/pad_sticks.h) and a seat's player in (players/seats.h; a seat
// plays its own pad, seat s pad s+1):
//   - after the game's Tick (its slot in the IInputProcessor vtable is replaced), every element's
//     previous/held bytes are rewritten to player 1's view: pad 1, keyboard and the game's own
//     synthetic keys, never a seated player's pad (input/button_view.h), except where the
//     game's own merge is meant (the menu openers, menu time: below). Everything that reads buttons
//     outside the seats' calls (player 1's input, the leader's move state, the menus, battle) sees
//     player 1. The Tick takes no player: it advances every player's view at once, so it needs no seat.
//   - a seated pad's action-downs reach the tables (for its player's polling) but are passed to the
//     game's dispatcher as already handled, its own flag, so that pad never runs a field prompt (or
//     the seat's go to the seat's own UI through the route below). The seat comes from the press's
//     source bit. This covers the XInput main loop, two DirectInput loops and, from the first input
//     Tick, the XInput extra-key calls (the shortcut keys), whose call sites cross a code block and
//     are changed from the game thread only;
//   - the pause menu is every pad's (RE-pause-menu-any-pad.md): the game's one open poll reads the
//     merged tables with no pad in mind, and its menus are driven through the dispatcher's UI route.
//     So the three menu-opening actions (PauseMenu, MapMenu, EnemyIntel) always get the game's own
//     merged state written back, and a seated pad's press of one is passed to the dispatcher with the
//     game's own flag; and while a pause-family menu owns the game's input (the game's pause states
//     or a pause-family widget alive, views/whole_screen.h MenuOwnsInput) every element gets the
//     merged state and every seated pad's press is passed as it came, outside the seat's own route:
//     every pad is the game's pad then, as in an unmodded game. A seat's shop (not a pause-family
//     menu) keeps its pad meanwhile. The merged stick already moves every menu cursor (pad_sticks
//     swaps only inside a player's own calls); releases are the game's own (its tables carry a source
//     mask per pad);
//   - a ButtonScope, opened around one seat's calls (its controller input and its member's tick, next
//     to the stick scope), writes that seat's view and puts back what the Tick wrote after. Seats take
//     turns: a scope never opens inside another.
// The repeat, latch and source bytes are never written. The game's own writers keep working on
// top: its press update (previous = held, held = 1) and release (when the mask empties) produce the
// same bytes a one-pad game would hold, and the next Tick rewrites every view.
//
// Per seat: each element's views (one per player, button_view::Views), the counters of the status
// line. Shared: the tables themselves (the game's), the menu player (one at a time). Game thread.
//
// Changes to held state or sources are also logged while a seat's player is in (capped per status line).

#include "platform/dev_switch.h"

#include <cstddef>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::input::pad_buttons {

/// Replaces the Tick slot after checking it. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// `seat`'s view of the buttons for the length of one engine call. Game thread, the seat's player in
/// and per-pad input on; does not nest (one seat's scope inside another's is refused). `withoutKey`
/// (an FName, 0 for none): an action the seat's view shows as not held for this call (a seat's ride
/// keeps the getoff button out: player 1 owns mounting).
class ButtonScope
{
public:
    explicit ButtonScope(int seat, std::uint64_t withoutKey = 0) noexcept;
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

/// A route for a seat's pad's presses to that seat's own UI (battle/menu_input_route.h finds the seat from
/// the source bit): true when it took the press (its dispatch result in `result`); false leaves the press kept from
/// the menus.
using SeatRouteFn = bool (*)(ActionDownFn dispatch, void* owner, void* unused, std::uint32_t sourceBit, void* event,
                             std::uint8_t alreadyHandled, char& result);
/// Which player's pad drives the menus (0 player 1, the default; a built seat while a shop its player opened is up,
/// world/vending): that player's view is the one the tables show outside the seats' calls, and the other pads'
/// presses are passed to the game's dispatcher as handled. Anything else than a built seat is player 1. Stick
/// navigation (the game's synthetic d-pad from the merged stick) is not split. Logged on change. Game thread.
void SetMenuPlayer(int seat) noexcept;

/// Sets the route (null: none). Game thread.
void SetSeatRoute(SeatRouteFn route) noexcept;

/// Diagnosis: which parts act (bits: 1 the split views, player 1's after the Tick and the seats' in
/// their calls; 2 a seated pad's presses kept from menus and prompts). Both at start. Logged.
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

}  // namespace lss::input::pad_buttons
