#pragma once

// A Slate widget's visibility set as the game sets it (SWidget::SetVisibility, an unbound attribute), for the HUD
// modules that show or withhold a game widget in one view's paint (hud/minimap/column_area_icons, hud/lead_mark).
// Only a widget whose visibility is unbound and whose vtable's slot is that function is set: anything else is left as
// the game has it. Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::widget_visibility {

/// Finds SWidget::SetVisibility (checked bytes). False (logged by the finder) if it differs.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// The widget can be set as the game sets it: unbound, and its vtable's slot is SWidget::SetVisibility.
bool Settable(std::uintptr_t widget) noexcept;

/// The widget's visibility value (`fallback` when unreadable).
std::uint8_t ValueOf(std::uintptr_t widget, std::uint8_t fallback) noexcept;

/// Sets `value` when it differs and the widget is settable. False when it was not set (not settable, or the call
/// raised).
bool Set(std::uintptr_t widget, std::uint8_t value) noexcept;

/// The widget a TSharedPtr / TWeakPtr at `pointer` holds while its reference controller's shared count is above 0;
/// else 0.
std::uintptr_t PinnedWidget(std::uintptr_t pointer) noexcept;

}  // namespace lss::hud::widget_visibility
