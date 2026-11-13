#pragma once

// A line of the mod's in the game's save-data notice, the box at the top right ("Checking saved data...";
// research/RE-title-notice.md): the one notice the game draws on the title screen, where neither the HUD's prompt nor
// the message window shows. The notice is the game's own: its show with the check's look, then the mod's text set on
// the widget's one text block with the game's setter, and the game's hide when the time is up. It has no text of its
// own to take and no duration, and the game's own save check uses the same widget: the mod waits while the game's
// notice is up, and shows its own again when the game's check hid it early. One line at a time; a new one replaces
// the old. Game thread.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::save_notice {

/// Finds the game's show, hide and text setter (checked; logged). False (nothing shows) if any differs.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// `text` in the notice for `seconds` from its first draw: at once when the game's notice is down or carries the mod's
/// own line, else when the game's goes down (within `seconds`, or the line is dropped).
void Show(std::wstring text, int seconds) noexcept;

/// Once per engine tick: the notice shown when due and the game's is down, shown again when the game's check hid it
/// early, hidden when its time is up.
void Tick() noexcept;

/// A line waits for its draw or is up.
bool Active() noexcept;

/// The line ended now, hidden if it is up, dropped if it waits (the title screen left: no line of its over a loading
/// screen). Game thread.
void End() noexcept;

}  // namespace lss::hud::save_notice
