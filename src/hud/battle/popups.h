#pragma once

// Battle popups (damage and heal numbers, battle texts such as Dodge, UMG popups such as level-up)
// placed in each player's view (RE-p2-battle-popups.md §1).
//
// The popup manager (EndUISystem+0xA0) stores the world's first player controller in each popup when
// it spawns, and places the popup through that controller every frame, so every view shows player 1's
// projection. Its tick cannot run again (count-up, state machine, pool returns), but both placement
// wrappers are pure apart from the popup's canvas slot offset and visibility. So:
//   - capture: while the game runs, each placement call is recorded with the arguments the game
//     passed (a few bytes at each wrapper's entry, hud/battle/popups_code.h);
//   - replay: in a seat's overlay pass (the seat's view rect set) each live popup is placed again by
//     the same wrapper with the seat's controller, and in player 1's re-run with the controller the
//     popup stored, which reproduces the game's own placement (the last pass of the frame is player
//     1's, as the next tick reads the UMG popups' visibility).
// The record is emptied after each frame's overlay render. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::popups {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// With the view's levers set: every popup the game placed this frame placed again, with `controller` (the view's: a
/// mirror view's for a copy of player 1's HUD), else the seat's, or for player 1 (`seat` 0, no controller) with its
/// own.
void Replay(int seat, void* controller = nullptr) noexcept;

/// After the overlay render: the record emptied for the next tick.
void EndFrame() noexcept;

}  // namespace lss::hud::popups
