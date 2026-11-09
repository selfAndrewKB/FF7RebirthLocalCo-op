#pragma once

// The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by every controller's menu
// (RE-p2-command-menu-tint.md). EnableType/DisableType are a latch over a real reference count, so two owners cancel
// each other; the game's other menus share a filter through the count itself (EndUIFilter_RefInc/RefDec on
// EndUISystem[41], keeping their own "holds it" byte). Each seat's menu does the same; player 1's keeps the latch.
// The wait sound's off is one mode slot: it is skipped while another menu is still in wait mode. A unit of the
// command menu tick (battle/command_menu_tick.h), which finds, places and removes it. Game thread.
// Per seat: each seat's menu holds its own count (by seat; the seat from the pass in flight); the wait sound waits
// for every other menu (menu_rules::SkipWaitSoundOffAmong).
//
// The slow-down switch (command_menu_tick::MenuSlow, the menu-slow setting; RE-menu-slow.md): switched off, a menu's
// wait raises neither the tint nor the muffle. The tint's on call is skipped (no latch and no count are taken, so its
// close and the game's restores after another menu find nothing to bring back), the muffle's two on calls (the sound
// event and the audio mode, inlined at the wait's open) are skipped, and its off is skipped too. A switch while menus
// wait is followed once, at the next controller tick, with the game's own calls (menu_rules::GreyAtSwitch,
// SoundAtSwitch). The rumble mute and the animations' blend floor of a waiting menu stay the game's.

#include "platform/dev_switch.h"

namespace lss::platform {
class DeferredSites;
class GameImage;
}

namespace lss::battle::menu_tint {

/// Finds the filter's and the sound's functions (each checked). False: the stage is unavailable.
bool Find(const platform::GameImage& image) noexcept;

/// Places the five call redirects (the wait mode's filter on and off, its sound off, the muffle's two on calls) through
/// the tick's placement. False if one failed (the tick then removes everything).
bool Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept;

/// `seat`'s count of the tint given back (its menu closed, or it stops holding for another reason). Logged.
void Release(int seat, const wchar_t* why) noexcept;

/// `seat`'s menu holds one count of the tint.
bool Holds(int seat) noexcept;

/// Gives every seat's count back and puts the five calls back (one changed after us is left in place, logged).
void Remove() noexcept;

/// The slow-down switch changed since the last call: every waiting menu's tint and the muffle follow it, once, with the
/// game's own calls. The controller tick calls it outside every pass.
void FollowSwitch() noexcept;

#if LSS_DEV
/// The `menu-tint` reading: the switch, the tints and muffles skipped, the switches followed. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::battle::menu_tint
