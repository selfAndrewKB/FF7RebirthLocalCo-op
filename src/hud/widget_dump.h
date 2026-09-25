#pragma once

// `dump widgets`: what the HUD's widgets report about themselves, through the engine's own
// reflected functions (UWidget::IsVisible, GetDesiredSize, GetCachedGeometry), for every live
// UUserWidget that is visible and was arranged. One log line each, largest first, capped. It only
// reads: the functions are const in the engine. Game thread only.

namespace lss::hud {

/// Logs the widgets; returns how many were listed (negative if the functions cannot be found).
int DumpWidgets() noexcept;

}  // namespace lss::hud
