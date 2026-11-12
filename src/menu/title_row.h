#pragma once

// The Co-op row of the title menu, directly below Options, so a player sees it on starting the game
// (research/RE-coop-page-menus.md, research/RE-coop-rows-facts.md; the page it opens: menu/coop_page, its opening:
// menu/coop_page_open).
//
// The start menu keeps its rows as a list of kinds and goes by kind for text and press. The demo's "Buy Now" kind (9) is
// built only under the demo check, so the full game never shows it: the mod gives it the row's text, takes its press,
// and after the start menu's construct inserts it below Options, then repeats the construct's own count, refresh and
// height calls, all before the game selects a row or plays the rows in. The kind's handler is replaced for good, so the
// trial's code never runs. A list that is not rebuilt keeps its row; a rebuilt one gets it again. Unplaced at an
// unload, a start menu already on screen keeps the row, showing the game's text and doing the game's press. Independent
// of the pause menu's row: a failed check here leaves the title as it was and logs why. Each insert logged at Debug,
// each failure at Warning. Game thread.
//
// The row takes kind 9, a kind the game builds but leaves spare; the title has four base rows; the list is found on the
// start menu by its own name lookup; the press is taken at the switch's one call of the kind's handler (1.005's handler
// entry crosses a block).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::menu::title_row {

/// False (logged) if a function or table is not where the facts say.
bool Install(const platform::GameImage& image) noexcept;
/// The press, the text, then the construct's slot, so the row never shows without its press. First tick, game thread.
bool Place(const platform::GameImage& image) noexcept;
void Unplace() noexcept;

/// The row's state, for the `menu.coop` reading ("; title row ...").
std::wstring Reading();

}  // namespace lss::menu::title_row
