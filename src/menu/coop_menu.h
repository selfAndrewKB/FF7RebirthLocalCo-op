#pragma once

// The Co-op row of the game's pause main menu and the page it opens (research/RE-coop-page-menus.md,
// research/RE-coop-rows-facts.md; the rows and texts: menu/coop_menu_rules.h; the page: menu/coop_page; its opening
// and closing: menu/coop_page_open).
//
// The row: the main menu's first page builds its left list from the kinds a builder appends, in the order of one word,
// and draws each kind's text and help from two tables. The mod gives the trial's "Buy Now" kind (13, which the full game
// never shows) its text and help, puts it before System in the order, and makes the builder append it: in 1.004 the name
// global it reads only there is written empty, in 1.005 its one call of the shared demo-mode test answers yes there
// only. So the game builds, draws, selects and remembers the row itself. A press of it reaches the game's window
// opener, where the mod's redirect opens the page instead of the trial's action. The page is the game's Options screen
// with the settings table's options, driven by the game's list, value popup, help line, sounds, animations and focus
// layer, so every pad does under the menu-time rule; a change applies at once and is saved.
// Install checks every part's functions (menu/game_ui, menu/coop_page, menu/coop_page_open) or places nothing. The
// redirects and the table words are placed on the first tick (game thread), the row's tables last, so the row never
// shows without its opener. Each tick runs the page's queued changes, then watches the open page's close. Reading
// `menu.coop`. Every step logged at Info. Game thread.
//
// Taken from Remake's menu/coop_menu. Changed: kind 13 (12 in Remake); the builder's gate per build (Remake writes an
// unlock slot).

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_menu {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick on the game thread.
void Tick() noexcept;

#if LSS_DEV
/// The `menu.coop` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::menu::coop_menu
