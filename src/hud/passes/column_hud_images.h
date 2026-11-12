#pragma once

// The further columns' HUD images: the game's own overlay rendered once more each frame per further column with a
// pass, each into a layer entry of its own in the same pool-2 slot, and each view family drawing only its own column's
// entry (research/RE-p2-hud-second-image.md; per column). The engine
// keeps one image per slot, so no game path makes a second one: the slot reuse, the skipped submits and the draw filter
// are the mod's, around the game's own render.
//
// Game thread, from the overlay render call (hud/passes/root_overlay_render.h, the one driver of the passes, which
// calls the steps below in order; the pass is hud/passes/overlay_pass.h, set there; the slot hud/passes/shared_slot.h):
//   1. each further column's pass, highest column first: SEndRootOverlay__RenderToCompositeLayer(overlay), with
//      DeltaTime 0. The first one's slot acquire is the real one and is held, the later ones take the held slot; each
//      one's layer entry is named for its column (hud/layout/overlay_layout.cpp asks EntryNameForPass); its submit is
//      skipped;
//   2. column 0's pass: the game's own call. Its acquire returns the held slot, its entry is the stock one, and its
//      submit sends the slot, now holding every further column's entry and player 1's.
// Player 1 renders last, so everything the overlay keeps from its last pass (its rects, the
// hit-test grid, the window size) is player 1's, as with every seat out.
//
// Render thread, per view family: the family's view names its player (FSceneView::PlayerIndex, the controller id, a
// seat's or a mirror's; read at the call that adds the composite passes), whose column this frame's
// columns map gives (published at column 0's pass). A further column with an entry this frame draws its own (which
// holds player 1's HUD while the column shows player 1's picture on a monitor of another shape); every other view
// (column 0's, a column with no entry) draws column 0's. The consume loop then gives each further column's entry its
// own vertex/index buffers and draws only the family's own entry (hud/passes/entry_filter_code.h).
//
// Each seat's own content in its pass comes from the levers of research/RE-p2-hud.md (hud/passes/seat_hud_content.h).
#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallColumnHudImages(const platform::GameImage& image) noexcept;
void RemoveColumnHudImages() noexcept;
bool ColumnHudImagesInstalled() noexcept;

/// The `hud-image` switch: the seats' HUD images on or off (on from a successful install).
void SetColumnHudImages(bool on) noexcept;
bool ColumnHudImagesOn() noexcept;

/// The frame's steps, game thread, from the overlay render call (hud/passes/root_overlay_render.cpp) in this order:
/// the slot's frame begins, unheld.
void BeginFrame() noexcept;

/// The further column's image renders this frame: `shown` (the column has a pass: a seat's own view on screen, or a
/// column on player 1's picture on a monitor of another shape, hud/passes/overlay_pass.h PassKindOf), the names and the
/// column's buffers ready (a wrong buffer kind refuses the images for good), the overlay with a window. `shownSeat`:
/// whose HUD the pass shows (0 player 1's laid out for the column's view; for the log and the render thread).
bool ImageReady(int column, bool shown, int shownSeat) noexcept;

/// A further column's pass begins, for the column ImageReady last answered for: the frame held (HoldFrame) for its
/// render, the slot taking part. Returns the frame's DeltaTime (for the updates run in the pass).
double BeginColumnImage() noexcept;

/// The engine's DeltaTime held at 0 and GFrameNumber one back, as for a further pass, without the slot: for player 1's
/// re-run outside a pass (hud/passes/seat_hud_content.h SettleSeats). Returns the frame's DeltaTime. ReleaseFrame
/// must follow. Only once the images are installed (a pass ran this frame).
double HoldFrame() noexcept;
void   ReleaseFrame() noexcept;

/// The game's overlay render for the pass's entry (SEndRootOverlay__RenderToCompositeLayer). False on a fault,
/// which refuses the images for good.
bool RenderColumnImage() noexcept;

/// The further pass ends: DeltaTime and the frame number back.
void EndColumnImage() noexcept;

/// The game's own call follows: marks which columns' entries were rendered and publishes this frame's view-to-column
/// table (the render thread reads both).
void BeginPlayerOnePass() noexcept;
/// Any further column's entry was rendered this frame.
bool ImageThisFrame() noexcept;

/// After the game's own call: a slot column 0's pass did not take is submitted on its own (logged).
void EndFrame() noexcept;

/// For the overlay's layer-entry call: the FName to use for `slot` in this pass (the column's for the held slot in a
/// further pass, else `stockName` unchanged).
std::uintptr_t EntryNameForPass(const void* slot, std::uintptr_t stockName) noexcept;

/// For the overlay's submit call: false for the held slot in a further pass (the slot is submitted once, by column
/// 0's pass).
bool SubmitInThisPass(const void* slot) noexcept;
#if LSS_DEV
/// The `hud-image` command and reading. May throw (out of memory).
void RegisterColumnHudImagesCommands();
#endif

}  // namespace lss::hud
