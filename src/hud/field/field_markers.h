#pragma once

// Each view shows only its own player's field-action point markers (RE-p2-field-interact.md §12.15; per seat since
// plan/seats.md step 4).
//
// With each seat's own marker widget per point (player_two/field_pickup), every player's widgets sit in the one
// marker list (EndUISystem+0x98) and every HUD run projects all of them (L1 gives the list the seat's context in the
// seat's run). So before each view's render the other players' point markers get render opacity 0 through the
// game's own setter (players/widget_opacity.h), and their own opacity back after: the projection sets only Visibility,
// so the two do not fight, and a widget kept painted keeps ticking. Markers that are not a point's (other owners'
// own widgets) stay in every view. Logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::field_markers {

/// Finds the marker list. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Hides, for the render pass about to run, the point markers that are not the seat's (0 player 1's). Restores first.
void ShowOnly(int seat) noexcept;

/// Every marker ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::hud::field_markers
