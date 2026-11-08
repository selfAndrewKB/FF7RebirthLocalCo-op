#pragma once

// The Co-op page itself: the mod's own instance of the game's Options screen, filled with the settings table's options,
// and its value reads and commits (research/RE-coop-page-options-screen.md, research/RE-coop-page-facts.md; the rows and
// texts: menu/coop_menu_rules.h; opened and closed by menu/coop_page_open).
//
// The page is the mod's own (not the one the System window keeps), made at the first open and kept while alive. A page
// out of the overlay is added again at the next open and its construct runs again over the game's data; the mod's
// categories, rows and left list then replace the game's through the game's own helpers (the row's constructor, move
// and destructor, the arrays' growth and emptying, the scroll box's count and refresh), each element counted only once
// built. A fault while writing marks the page failed: the row opens nothing until the game restarts. The game's value
// read and commit are redirected for the mod's rows only (their category bytes from 0x80, which the game never makes):
// a read answers the option's current word's place, a commit while the page is open is queued and the next tick runs
// the option's console command and saves it to the settings file, so it applies at once and at the next start. Each
// made page and each change logged at Info, each commit at Debug, each value read at Trace. Game thread.
//
// Taken from Remake's menu/coop_page. Changed: a category's rows are emptied by the game's move-assign from an empty
// array (this game has no ranged remove); the overlay add writes the page's order itself; 1.005's value read is entered
// through its second instructions (its first crosses a block); a commit is saved to the settings file. Rows with more
// values than the game's longest list need the game's slider row (menu/coop_slider), which this page does not build
// yet: they stay on the console and in the settings file.

#include "menu/coop_menu_rules.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_page {

/// Finds the page's functions and builds its categories. False if a check failed or the table has no options.
bool Install(const platform::GameImage& image) noexcept;
/// The value read and commit redirected (first tick, game thread). False if either could not be placed.
bool Place(const platform::GameImage& image) noexcept;
void Unplace() noexcept;

/// The page's categories and rows, in order.
const std::vector<coop_menu_rules::Category>& Categories() noexcept;

/// The mod's page, made if needed (from `host`'s world, with the game's `inGame`: 1 from the main menu, 0 from the
/// title), not yet added or written; `inRoot` says whether it is in the overlay already. Null (logged) if it could not
/// be made, or while it still plays its close. A page made for the other kind of host is forgotten and a new one made.
void* Prepare(void* host, std::uint8_t inGame, bool& inRoot) noexcept;
/// The mod's data written into `page` (after its construct ran: once in the overlay) and its title set. False
/// (logged, the row failed until the next start) if writing failed: the caller takes the page down.
bool Fill(void* page) noexcept;
/// Prepare, the page added to the root overlay at `z` if it is not in it, then Fill: the page ready to open, or null.
void* Ready(void* host, std::int32_t z, std::uint8_t inGame) noexcept;
/// The page taken out of the overlay the way its own no-animation Back does (state 0), its data forgotten: after a
/// failed open, or with its focus layer already popped (menu/coop_page_open's close with the main menu).
void Abandon(void* page) noexcept;
/// The mod's page while alive, else null.
void* Page() noexcept;
/// `object` is the mod's page (by its weak pointer: a dead page's address may hold another object).
bool IsPage(const void* object) noexcept;

/// The page is open (set by menu/coop_page_open): commits are taken only then.
bool IsOpen() noexcept;
void SetOpen(bool open) noexcept;

/// Writing the page failed once: the row opens nothing until the game restarts.
bool Failed() noexcept;
/// Changes run since the start.
std::uint64_t Commits() noexcept;

/// Once per engine tick on the game thread: forgets the data of a page that is gone, then runs the queued commits.
void Tick() noexcept;

}  // namespace lss::menu::coop_page
