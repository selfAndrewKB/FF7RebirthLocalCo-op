#pragma once

// The widgets the game puts on the screen, as it adds them to its root overlay (Square Enix's
// UEndUserWidget::AddToRootOverlay, the ZOrderPlus10 body every add goes through; research/RE-p2-menu-census.md E5).
//
//   - Each widget class is logged the first time it is added (Debug: class, z), later adds at Trace: the log names the
//     screens no native code reads, so a family the single view misses can be told apart in one play session.
//   - The full-screen Blueprint menus no native code reads (Chadley's VR battle list, the colosseum list, photo mode;
//     E6) are followed while they stay on screen: alive (the game's own weak-pointer resolve), still in the overlay
//     (the game's z field +0x2A8, which its RemoveFromParent override sets to -1) and not Collapsed or Hidden
//     (UWidget::Visibility). While one is, views/whole_screen gives the single view.
//
// Game thread only (the game adds widgets on it).

namespace lss::platform {
class GameImage;
}

namespace lss::views::screen_widgets {

/// Redirects the add; false (logged) if its first instruction is not what the research read.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, once per tick: whether a followed full-screen menu is on screen now. Logs each one closing.
bool FullScreenOpen() noexcept;

}  // namespace lss::views::screen_widgets
