#pragma once

// A seat's pad's presses and releases to the seat's menu in battle (RE-p2-command-menu-input.md §3.3, §10 Stage 3d). A
// press runs through the game's own dispatcher in the seat's UI (its focus context and scopes, the pass in flight
// marked 3), so its route, list and command-menu handler act on the seat's menu; only in battle, with the seat's
// menu layer in place and player 1's top layer the command menu's own. A release follows its press (the capture
// map's rule): the key-up handler notes which pad it came from, and the up route's call walks the seat's focus
// context for a release whose press went there, whatever is open then. A unit of the command menu tick
// (battle/command_menu_tick.h), which finds, installs and removes it. Game thread.
//
// Per seat: a press goes to the UI of the seat whose pad its source bit is, a release to the
// UI of the seat whose pad it came from; each seat keeps its own set of taken keys, so two seats holding the same
// action keep apart.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::menu_input_route {

/// Finds the action keys the route and its log compare an event's key with (each checked). False: unavailable.
bool Find(const platform::GameImage& image) noexcept;

/// Sets the further pads' route for presses, and wraps the key-up handler and the up route's call for releases
/// (optional: without it their releases keep running player 1's up route). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Clears the route and puts the key-up slot and the up route's call back (one changed after us is left, logged).
void Remove() noexcept;

}  // namespace lss::battle::menu_input_route
