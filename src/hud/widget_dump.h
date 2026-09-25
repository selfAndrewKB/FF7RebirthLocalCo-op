#pragma once

// `dump widgets`: what the HUD's widgets report about themselves, through the engine's own
// reflected functions (UWidget::IsVisible, GetDesiredSize, GetCachedGeometry), for every live
// UUserWidget that is visible and was arranged. One log line each, largest first, capped. It only
// reads: the functions are const in the engine. Game thread only.

#include <string>

namespace lss::hud {

/// Logs the widgets; returns how many were listed (negative if the functions cannot be found).
int DumpWidgets() noexcept;

/// `dump slots <Class>`: for each live UUserWidget of that class, walks its WidgetTree (RootWidget,
/// then panels' Slots, nested user widgets' trees, a few levels deep) through reflection and logs
/// every panel slot: its class, its content, and for canvas slots the LayoutData (offsets,
/// anchors, alignment) and bAutoSize. Returns the number of slots logged, negative on failure.
int DumpSlots(const std::wstring& className) noexcept;

}  // namespace lss::hud
