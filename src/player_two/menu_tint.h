#pragma once

// The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by both controllers' menus
// (RE-p2-command-menu-tint.md). EnableType/DisableType are a latch over a real reference count, so two owners cancel
// each other; the game's other menus share a filter through the count itself (EndUIFilter_RefInc/RefDec on
// EndUISystem[41], keeping their own "holds it" byte). Player 2's menu does the same; player 1's keeps the latch.
// The wait sound's off is one mode slot: it is skipped while the other menu is still in wait mode. A unit of the
// command menu tick (player_two/command_menu_tick.h), which finds, places and removes it. Game thread.

namespace lss::platform {
class DeferredSites;
class GameImage;
}

namespace lss::player_two::menu_tint {

/// Finds the filter's and the sound's functions (each checked). False: the stage is unavailable.
bool Find(const platform::GameImage& image) noexcept;

/// Places the three call redirects (the wait mode's filter on and off, its sound off) through the tick's placement.
/// False if one failed (the tick then removes everything).
bool Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept;

/// Player 2's count of the tint given back (its menu closed, or it stops holding for another reason). Logged.
void Release(const wchar_t* why) noexcept;

/// Player 2's menu holds one count of the tint.
bool Holds() noexcept;

/// Gives player 2's count back and puts the three calls back (one changed after us is left in place, logged).
void Remove() noexcept;

}  // namespace lss::player_two::menu_tint
