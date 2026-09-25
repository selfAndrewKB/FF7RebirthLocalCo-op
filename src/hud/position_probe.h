#pragma once

// A logging probe on how HUD widgets place themselves from Blueprints (RE-p2-hud.md pass 4): the
// field command menu and its party switcher are children of the HUD overlay, but no native code
// sets their position, so their Blueprints do. Their only native way in is
// UUserWidget::execSetPositionInViewport, whose one call to SetPositionInViewport (0x144CED910)
// is redirected here (platform/call_redirect.h) to log, with the probe on, each widget's class,
// the position and DPI flag it asked for, and its viewport-slot anchors, offsets and alignment.
// It changes nothing: the call goes through exactly as it came.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallPositionProbe(const platform::GameImage& image) noexcept;
void RemovePositionProbe() noexcept;
bool PositionProbeInstalled() noexcept;

/// The `hud-probe` switch (off at start). Each switch-on logs up to kProbeLines lines.
void SetPositionProbe(bool on) noexcept;

}  // namespace lss::hud
