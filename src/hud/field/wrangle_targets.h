#pragma once

// The chocobo wrangle's targets in each view: placed from that view's camera (RE-chocobo-wrangle-targets.md; seen
// in play: the "..." bubbles at player 1's screen spots in every column of the row).
//
// The capture controller (g_EndChocoboCaptureManager+0x18) keeps one entry per chocobo (392 bytes: the chocobo, a
// state byte, the Status_00 bubble, its small form and the arrow) and the throw reticle (Target_C) over the current
// target. Native code places them once per frame from the world tick through the party manager's primary
// controller (PM+0x140): one shared projection helper (a world point to the root overlay, the edge clamp and the
// arrow's angle), then the widget's render transform. The HUD passes draw that one placement in every view.
//
// The placement is stateless (the bubbles: visibility and transform from the entry's state byte; the reticle: the
// target location the game stored), so each seat's pass runs it again once the pass holds the seat as the primary
// controller (L3), and player 1's pass again after it: the game's own per-entry placer for the bubbles, and the
// helper plus the render-transform apply for the reticle (the game's own target placer would pick the target again
// and restart its animation). Nothing is hidden: the wrangle is one shared activity, every view shows the same
// targets from its own camera. The chicken lure places the same widget classes through its own copies of these
// functions and its own holder (unmapped): not covered. The retry-point notice and the command pane are screen-fixed.
// Game thread only. Logged at Debug on change.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::wrangle_targets {

/// Finds the capture manager, the placer, the projection helper and the render-transform apply. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Every bubble and the reticle placed again through the primary controller the pass holds (`seat` 0: player 1's
/// after a seat's pass, or a mirror's held around this call in its copy). Nothing while no wrangle is up.
void Place(int seat) noexcept;

}  // namespace lss::hud::wrangle_targets
