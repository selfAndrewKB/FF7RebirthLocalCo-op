#pragma once

// The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by every controller's menu
// (RE-p2-command-menu-tint.md). EnableType/DisableType are a latch over a real reference count, so two owners cancel
// each other; the game's other menus share a filter through the count itself (EndUIFilter_RefInc/RefDec on
// EndUISystem[41], keeping their own "holds it" byte). Each seat's menu does the same; player 1's keeps the latch.
// The wait sound's off is one mode slot: it is skipped while another menu is still in wait mode. A unit of the
// command menu tick (battle/command_menu_tick.h), which finds, places and removes it. Game thread.
// Per seat: each seat's menu holds its own count (by seat; the seat from the pass in flight); the wait sound waits
// for every other menu (menu_rules::SkipWaitSoundOffAmong).

namespace lss::platform {
class DeferredSites;
class GameImage;
}

namespace lss::battle::menu_tint {

/// Finds the filter's and the sound's functions (each checked). False: the stage is unavailable.
bool Find(const platform::GameImage& image) noexcept;

/// Places the three call redirects (the wait mode's filter on and off, its sound off) through the tick's placement.
/// False if one failed (the tick then removes everything).
bool Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept;

/// `seat`'s count of the tint given back (its menu closed, or it stops holding for another reason). Logged.
void Release(int seat, const wchar_t* why) noexcept;

/// `seat`'s menu holds one count of the tint.
bool Holds(int seat) noexcept;

/// Gives every seat's count back and puts the three calls back (one changed after us is left in place, logged).
void Remove() noexcept;

}  // namespace lss::battle::menu_tint
