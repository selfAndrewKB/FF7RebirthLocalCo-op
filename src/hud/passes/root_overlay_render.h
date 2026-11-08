#pragma once

// The moment just before the HUD overlay renders, each frame.
//
// UGameEngine::Tick calls FSlateApplication__ExecuteDelegate1000_RenderRootOverlay once per
// frame (0x1410A0156), after the world tick in which the game updates its HUD (MasterHUD_Tick) and
// moves its widgets, and before the overlay lays the HUD out and draws it (RE-p2-hud.md P8.9,
// P8.10). That call is redirected here (platform/call_redirect.h). Before passing it through, on
// the game thread:
//   - the HUD placement places again any widget the game moved in this frame's world tick
//     (hud/layout/hud_placement.h), so the frame draws it placed rather than at its single-player point;
//   - the HUD trace notes the render (hud/hud_trace.h), and after it logs the frame;
//   - each further column's pass runs first, highest column first, when its image is on: the levers and per-pass
//     opacities of the HUD modules around the game's overlay render into the column's slot entry
//     (hud/passes/column_hud_images.h holds the slot and renders; hud/passes/overlay_pass.h names the pass); the game's
//     own call is column 0's pass, player 1's, last. This file is the one driver of the passes.
// The call itself goes through unchanged.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallRootOverlayRender(const platform::GameImage& image) noexcept;
void RemoveRootOverlayRender() noexcept;
bool RootOverlayRenderInstalled() noexcept;

}  // namespace lss::hud
