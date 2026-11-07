#pragma once

// Each player's own minimap label text and layout (hud/minimap/label_states.h; RE-p2-minimap-cost.md §7).
//
// The one call that sets an icon label's text, SEndMapIcon::SetLabelText → SEndTextBlock SetText (0x14098EBB1), is
// redirected. Inside the mod's holder run for a seat only, the seat's last state is swapped into the text block first;
// after the seat's paint it is swapped out again. Each seat keeps its own states (plan/seats.md step 4). Anywhere else
// the call goes straight to the game's. Optional: if a check fails, the labels are not swapped (a relayout per pass
// each frame while a seat's minimap is on). Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap_labels {

/// After the minimap's own install. Logs either way.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the call back; on the game thread, also each block's player 1 state and every record (else they are kept).
void Remove() noexcept;

/// Around the mod's holder run for a seat (hud/minimap/minimap.cpp, RunHolder).
void BeginRun(int seat) noexcept;
void EndRun() noexcept;

/// After a seat's paint, before player 1's holder run: player 1's state back into every block.
void SwapBack() noexcept;

/// Each engine tick, outside the overlay render: frees the records of labels that died; for a seat whose minimap is
/// off (its bit clear in `seatsOn`), frees them all. Logs the counts every two minutes.
void Tick(unsigned seatsOn) noexcept;

}  // namespace lss::hud::minimap_labels
