#pragma once

// Logs the minimap's objects as the game holds them (read-only; groundwork for player 2's own minimap).
//
// The area-map manager (EndUISystem +0x108) holds the navimap holder (+0x340); its interface (+0x20) is the
// UEndNaviMapCircle/Octagon widget's native interface (UObject +0x108), whose Slate widget (+0x138) paints the
// map into its own render target (+0x490) and holds the map content widget SEndNaviMap (+0x3B0) with the centre,
// zoom and rotations the holder writes each frame (RE-p2-own-minimap.md §1, §3). A line is written when the
// widget, its kind, its target or the holder's rotation flags change, and every 30 s while the split is shown
// (centre and rotations). Nothing is written to the game.
// Game thread.

#include "platform/image.h"

namespace lss::hud::navimap_watch {

void Install(const platform::GameImage& image) noexcept;

/// Each engine tick.
void Tick(bool splitShown) noexcept;

}  // namespace lss::hud::navimap_watch
