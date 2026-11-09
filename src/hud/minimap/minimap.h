#pragma once

// Each seat's own minimap: its centre, rotation and icons, in its view (RE-p2-own-minimap.md,
// "Recommended design" and §8; per seat since plan/seats.md step 4).
//
// The minimap widget (SEndNaviMapCircle) renders its map into a render target through the menu renderer's pool 0,
// then shows that target in the HUD. Its content (centre, rotations, icons) is written each frame by the game's
// navimap holder update from the party leader and the primary camera. Nothing here re-implements any of it:
// - Content: the holder update is run again for each player inside the HUD levers (as the compass is). In a seat's
//   run, its one leader-type read answers the seat's member's type (the camera read already follows the
//   primary-controller lever). The holder's own rotation switch (the Static/Dynamic option) still decides.
// - Target: one more UTextureRenderTarget2D per seat, made with the game's own calls (NewObject, InitCustomFormat,
//   UpdateResourceImmediate) at the live target's size and rooted. It is swapped into the widget's target field
//   around the seat's overlay render only, which moves the entry's target and the HUD element that samples it.
// - Pool 0: one acquire and one submit per frame, as with one player. The first seat pass acquires and holds the
//   slot, each later one takes it; each names its entry for its seat ("SEndNaviMapCircle_P2", "_P3") and skips its
//   submit; player 1's pass gets the held slot back, adds the stock entry and submits them all; the consume draws each
//   entry into its own target. If player 1's pass does not paint the minimap, the held slot is submitted after it with
//   the game's own function (a held slot would hang the next acquire). Each seat's entry is drawn from its own vertex
//   and index buffers (hud/passes/entry_filter_code.h).
// Until every part is ready a seat's view keeps the earlier behaviour (player 1's map, hud/minimap/minimap_once.h).
// Game thread, except the consume code (render thread).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, outside the overlay render: installs the acquire redirect (from this thread), makes each seat's
/// target and buffers when needed, decides which seats' minimaps this frame uses, and logs.
void Tick(bool splitShown) noexcept;

/// From the HUD levers' update runs (hud/passes/seat_hud_content.h): the navimap holder update for that player's view
/// (`seat` 0 player 1's, run only after a seat's).
void RunHolder(int seat) noexcept;

/// The pin of `key` on the minimap withheld from this paint: its icon set Hidden as the game sets it. The holder's next
/// run sets it again, so it is asked after each run, before the paint. False when the holder has no such pin (or it
/// could not be set).
bool HidePin(std::uint64_t key) noexcept;

/// Around a seat's overlay render. True if the seat's minimap is painted in it (the minimap-once skip is
/// then off for that pass).
bool BeginPaint(int seat) noexcept;
void EndPaint() noexcept;

/// Before the overlay render's passes: the circle's slot is unheld.
void BeginOverlayRender() noexcept;

/// After player 1's overlay render: submits a slot player 1's pass did not take.
void EndOverlayRender() noexcept;

}  // namespace lss::hud::minimap
