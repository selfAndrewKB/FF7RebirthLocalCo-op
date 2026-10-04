#pragma once

// Each half shows only its own player's field-action point markers (RE-p2-field-interact.md §12.15).
//
// With player 2's own marker widget per point (player_two/field_pickup), both players' widgets sit in the one
// marker list (EndUISystem+0x98) and both HUD runs project all of them (L1 gives the list player 2's context in
// player 2's run). So before each half's render the other player's point markers get render opacity 0 through the
// game's own setter (hud/widget_opacity.h), and their own opacity back after: the projection sets only Visibility,
// so the two do not fight, and a widget kept painted keeps ticking. Markers that are not a point's (other owners'
// own widgets) stay in both halves. Logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::field_markers {

/// Finds the marker list. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Hides, for the render pass about to run, the point markers that are not `player`'s (0 or 1). Restores first.
void ShowOnly(int player) noexcept;

/// Every marker ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::hud::field_markers
