#pragma once

// Player 2's HUD image: the game's own overlay rendered a second time each frame, into a second
// layer entry of the same pool-2 slot, and each view family drawing only its own player's entry
// (RE-p2-hud-second-image.md; the mechanism is §5 there). The engine keeps one image per slot, so
// no game path makes a second one: the slot reuse, the skipped submit and the draw filter are the
// mod's, around the game's own render.
//
// Game thread, from the overlay render call (hud/root_overlay_render.h), each frame while player 2
// is in and the image is on (the pass is hud/overlay_pass.h, set here; the slot hud/shared_slot.h):
//   1. player 2's pass: SEndRootOverlay__RenderToCompositeLayer(overlay), with DeltaTime 0. Its
//      slot acquire is the real one and is held; its layer entry is named for player 2
//      (hud/overlay_layout.cpp asks EntryNameForPass); its submit is skipped;
//   2. player 1's pass: the game's own call. Its acquire returns the held slot, its entry is
//      the stock one, and its submit sends the slot, now holding [player 2's, player 1's] entries.
// Player 1 renders last, so everything the overlay keeps from its last pass (its rects, the
// hit-test grid, the window size) is player 1's, as without player 2.
//
// Render thread, per view family: the family's view says which player it is (its view rect at the
// call that adds the composite passes, 0x140FDE3C1); the consume loop then gives player 2's entry
// its own vertex/index buffers and draws only the family's own entry (hud/entry_filter_code.h).
//
// Player 2's own content in their pass comes from the levers of RE-p2-hud.md P8.6 (hud/player_two_pass.h).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallSecondImage(const platform::GameImage& image) noexcept;
void RemoveSecondImage() noexcept;
bool SecondImageInstalled() noexcept;

/// The `hud-image` switch: player 2's HUD image on or off (on from a successful install).
void SetSecondImage(bool on) noexcept;
bool SecondImageOn() noexcept;

/// Game thread, from the overlay render call: renders player 2's image first (if on and the split
/// is on screen: player 2 in, and not player 1's single view), then marks the game's own call that
/// follows as player 1's pass.
void BeginOverlayRender(bool splitShown) noexcept;

/// Game thread, after the game's own call.
void EndOverlayRender() noexcept;

/// For the overlay's layer-entry call: the FName to use for `slot` in this pass (player 2's for the held slot in
/// player 2's pass, else `stockName` unchanged).
std::uintptr_t EntryNameForPass(const void* slot, std::uintptr_t stockName) noexcept;

/// For the overlay's submit call: false for the held slot in player 2's pass (the slot is submitted once, by
/// player 1's pass).
bool SubmitInThisPass(const void* slot) noexcept;

}  // namespace lss::hud
