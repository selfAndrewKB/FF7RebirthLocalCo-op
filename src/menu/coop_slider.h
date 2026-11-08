#pragma once

// The Co-op page's sliders (research/RE-coop-page-facts.md §1.2, §1.3, §3): an option with more values than the game's
// longest list (its value list cannot scroll) is the game's own Range row (menu/coop_page builds it;
// coop_menu_rules::IsSlider), made to scroll. The game's gauge holds one cell per value and grows past its frame, since
// nothing sizes its strip; the strip is the game's scrolling list, so after each of the game's own gauge updates (the
// press and every Left/Right step, both calls redirected) the mod sizes the strip to the stock 10 cells
// (SetDesiredCount), scrolls it to keep the marker in view (SetScrollOffset, the nearest-edge rule of
// coop_menu_rules::FirstVisible), places the marker by the canvas slot's setter, and writes the value's word in the
// gauge's label, each through the game's reflected calls. The row's own value text is the word too (the row text's
// AppendInt call redirected for the mod's sliders). The mod deciding when to scroll is the one step the game has no
// path for (no gauge of the game's holds more than its frame), a recreation accepted for that reason. Holding
// Left/Right steps on: the page's action handler drops the input's repeats, so its vtable slot is replaced and on the
// mod's open slider a held d-pad Left/Right reaches it as a press, the event put back after. Accept commits through the
// page's key handler (menu/coop_page_open, coop_page::CommitEdit). Only the mod's page is touched. Each open logged at
// Debug, each step at Trace. Game thread.
//
// Taken from Remake's menu/coop_slider. Changed: the value text is AppendInt into an empty string (Remake: FromInt
// returning one); SetScrollOffset takes an animate flag; the event's repeat and key at this game's offsets; 1.004's
// gauge calls cross a block (changed from the game thread); no Slate-side readings.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_slider {

/// Finds the gauge's function, the row text's AppendInt and the reflected calls. False if one is missing (the long rows
/// then stay off the page).
bool Install(const platform::GameImage& image) noexcept;
/// The three call redirects (first tick, game thread). False if one could not be placed (then none is). Then the page's
/// action handler for held steps, on its own (logged; without it each press steps once).
bool Place(const platform::GameImage& image) noexcept;
void Unplace() noexcept;

/// The sliders are placed: the page builds its long rows as sliders.
bool Ready() noexcept;

}  // namespace lss::menu::coop_slider
