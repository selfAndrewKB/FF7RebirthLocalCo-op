#pragma once

// Player 2's minimap: player 2's centre, rotation and icons, in player 2's half (RE-p2-own-minimap.md,
// "Recommended design" and §8).
//
// The minimap widget (SEndNaviMapCircle) renders its map into a render target through the menu renderer's pool 0,
// then shows that target in the HUD. Its content (centre, rotations, icons) is written each frame by the game's
// navimap holder update from the party leader and the primary camera. Nothing here re-implements any of it:
// - Content: the holder update is run again for each player inside the HUD levers (as the compass is). In player
//   2's run, its one leader-type read answers player 2's member's type (the camera read already follows the
//   primary-controller lever). The holder's own rotation switch (the Static/Dynamic option) still decides.
// - Target: a second UTextureRenderTarget2D, made with the game's own calls (NewObject, InitCustomFormat,
//   UpdateResourceImmediate) at the live target's size and rooted. It is swapped into the widget's target field
//   around player 2's overlay render only, which moves the entry's target and the HUD element that samples it.
// - Pool 0: one acquire and one submit per frame, as with one player. Player 2's pass acquires and holds the slot,
//   names its entry "SEndNaviMapCircle_P2" and skips its submit; player 1's pass gets the held slot back, adds the
//   stock entry and submits both; the consume draws each entry into its own target. If player 1's pass does not
//   paint the minimap, the held slot is submitted after it with the game's own function (a held slot would hang
//   the next acquire). Player 2's entry is drawn from its own vertex and index buffers (hud/entry_filter_code.h).
// Until every part is ready the minimap keeps the earlier behaviour (player 1's map in both halves,
// hud/minimap_once.h). Game thread, except the consume code (render thread).

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, outside the overlay render: installs the acquire redirect (from this thread), makes player 2's
/// target and buffers when needed, decides whether this frame uses player 2's minimap, and logs.
void Tick(bool splitShown) noexcept;

/// From the HUD levers' update runs (hud/player_two_pass.h): the navimap holder update for that player's view.
void RunHolder(bool playerTwo) noexcept;

/// Around player 2's overlay render. True if player 2's minimap is painted in it (the minimap-once skip is
/// then off for that pass).
bool BeginPlayerTwoPaint() noexcept;
void EndPlayerTwoPaint() noexcept;

/// After player 1's overlay render: submits a slot player 1's pass did not take.
void EndOverlayRender() noexcept;

}  // namespace lss::hud::minimap
