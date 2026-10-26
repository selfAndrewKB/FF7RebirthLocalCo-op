#pragma once

// The further players' HUD images: the game's own overlay rendered once more each frame per further seat with a view,
// each into a layer entry of its own in the same pool-2 slot, and each view family drawing only its own player's entry
// (RE-p2-hud-second-image.md; the mechanism is §5 there; per seat since plan/seats.md step 4). The engine keeps one
// image per slot, so no game path makes a second one: the slot reuse, the skipped submits and the draw filter are the
// mod's, around the game's own render.
//
// Game thread, from the overlay render call (hud/passes/root_overlay_render.h, the one driver of the passes, which
// calls the steps below in order; the pass is hud/passes/overlay_pass.h, set there; the slot hud/passes/shared_slot.h):
//   1. each further seat's pass, highest seat first: SEndRootOverlay__RenderToCompositeLayer(overlay), with DeltaTime
//      0. The first one's slot acquire is the real one and is held, the later ones take the held slot; each one's
//      layer entry is named for its seat (hud/layout/overlay_layout.cpp asks EntryNameForPass); its submit is skipped;
//   2. player 1's pass: the game's own call. Its acquire returns the held slot, its entry is the stock one, and its
//      submit sends the slot, now holding every seat's entry and player 1's.
// Player 1 renders last, so everything the overlay keeps from its last pass (its rects, the
// hit-test grid, the window size) is player 1's, as without the further players.
//
// Render thread, per view family: the family's view names its player (FSceneView::PlayerIndex, the controller id,
// which is the seat; read at the call that adds the composite passes, 0x140FDE3C1). A seat with an entry this frame
// draws its own; every other view (player 1's, a view no seat fills) draws player 1's. So a view that is not a seat's
// (the mirror view, views/mirror_player.h, plan/seats.md step 5) must never carry a seat's controller id, or it would draw that
// seat's HUD; hud/hud_roots.cpp likewise takes the first listed player with each id as that seat's. The consume loop
// then gives each seat's entry its own vertex/index buffers and draws only the family's own entry
// (hud/passes/entry_filter_code.h).
//
// Each seat's own content in its pass comes from the levers of RE-p2-hud.md P8.6 (hud/passes/player_two_pass.h).
#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallSecondImage(const platform::GameImage& image) noexcept;
void RemoveSecondImage() noexcept;
bool SecondImageInstalled() noexcept;

/// The `hud-image` switch: the further players' HUD images on or off (on from a successful install).
void SetSecondImage(bool on) noexcept;
bool SecondImageOn() noexcept;

/// The frame's steps, game thread, from the overlay render call (hud/passes/root_overlay_render.cpp) in this order:
/// the slot's frame begins, unheld.
void BeginFrame() noexcept;

/// The seat's image renders this frame: `shown` (the seat's own view on screen: the split, not player 1's single view
/// and not player 1's picture in the seat's column, views/column_source.h), the names and the seat's buffers ready
/// (a wrong buffer kind refuses the images for good), the overlay with a window. `seat` is a built seat in the game.
bool ImageReady(int seat, bool shown) noexcept;

/// A further pass begins, for the seat ImageReady last answered for: the engine's DeltaTime held at 0 for its
/// render, the slot taking part. Returns the frame's DeltaTime (for the updates run in the pass).
double BeginFurtherPass() noexcept;

/// The game's overlay render for the pass's entry (SEndRootOverlay__RenderToCompositeLayer). False on a fault,
/// which refuses the images for good.
bool RenderFurther() noexcept;

/// The further pass ends: DeltaTime back.
void EndFurtherPass() noexcept;

/// The game's own call follows: marks whose entries were rendered (the render thread reads it).
void BeginPlayerOnePass() noexcept;
/// Any seat's entry was rendered this frame.
bool ImageThisFrame() noexcept;

/// After the game's own call: a slot player 1's pass did not take is submitted on its own (logged).
void EndFrame() noexcept;

/// For the overlay's layer-entry call: the FName to use for `slot` in this pass (the seat's for the held slot in a
/// further pass, else `stockName` unchanged).
std::uintptr_t EntryNameForPass(const void* slot, std::uintptr_t stockName) noexcept;

/// For the overlay's submit call: false for the held slot in a further pass (the slot is submitted once, by player
/// 1's pass).
bool SubmitInThisPass(const void* slot) noexcept;
#if LSS_DEV
/// The `hud-image` command and reading. May throw (out of memory).
void RegisterSecondImageCommands();
#endif

}  // namespace lss::hud
