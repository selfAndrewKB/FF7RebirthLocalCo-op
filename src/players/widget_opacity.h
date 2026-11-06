#pragma once

// A UMG widget's render opacity, set through the game's own native UWidget::SetRenderOpacity
// (RE-p2-hud-targets.md, S-HUD22): it writes the widget's RenderOpacity (+0xC4) and, when the Slate
// widget is built and its opacity differs, stores it there and invalidates paint. No UMG event runs.
//
// Used to take a widget out of one half's image while keeping it painted, so UMG still ticks it
// (its animations and fades advance): a widget that is Hidden or Collapsed in player 1's pass is
// never ticked with the real frame time, since a seat's pass runs with DeltaTime 0 (plan, v0.12.3).
// An engine path shared by the HUD modules and the seats' menu views (players/, below both). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::players::widget_opacity {

/// Finds the game's setter; a row of the module table before the modules that read it (they check Installed).
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// The widget's render opacity (1 if it cannot be read).
float Get(const void* widget) noexcept;

/// Sets it through the game's function. False if not installed or the call faulted.
bool Set(void* widget, float opacity) noexcept;

}  // namespace lss::players::widget_opacity
