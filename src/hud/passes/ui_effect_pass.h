#pragma once

// The UI particles (the summon gauge's glow, every Niagara effect whose render target is the composite foreground)
// placed and sized per column. The engine draws them per view family with the stock mesh-draw machinery into the
// family's composite-foreground target, the final pass blends that into the view, and the effect's own vertex path
// puts the particles in orthographic screen space (research/RE-ui-particle-pass.md §12). One view family per column
// under the mod, so once per column. The particle itself is one per effect (one position, one size for every
// column: the game converts its layout point once a frame, centred in the layout the overlay holds then and in its
// half-heights), but the pass sets a viewport over the whole target just before its draw, and a viewport is the one
// map every vertex goes through: set to a box of that layout's height at the column's band's scale and the target's
// shape, centred where that layout's centre and the widget's own move land in the column, it moves and scales every
// UI particle to the column's HUD at the band's scale, positions and sprite sizes together, whatever the shader did
// before (hud/layout/viewport_placement.h EffectViewport). The glow's design points and the
// effect's own sizes stay the game's. The lever is one call inside the pass's execute, the per-draw setup that
// rewrites the view's uniform buffer for the target and appends the viewport command: the mod's replacement runs the
// engine's own, then sets the command it appended to the box, after checking it holds the whole extent. The column
// is the family's, as the composite redirect (hud/passes/column_hud_images.cpp) decides it earlier on the same
// thread; the boxes come from the overlay's rect call of the frame's last pass (game thread, through a seqlock). On
// the research build the call crosses a 16-byte block, so the render thread installs it from the composite redirect
// at the first frame.

#include "hud/layout/viewport_placement.h"
#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::ui_effect_pass {

/// Checks the facts and installs the call redirect (or, where the call crosses a block, arms its install from the
/// render thread). False, with the reason logged, when a fact is not what the research read.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the engine's call back (from the game thread where the site allows it; else asked of the render thread, waited
/// for briefly, and left in place but inert when it does not come).
void Remove() noexcept;

/// The boxes act: installed, switched on, not taken down, and seen to set a viewport. The glow's design points and the
/// effect's sizes are then left the game's (hud/layout/hud_placement.cpp, hud/layout/glow_size.h).
bool Active() noexcept;

/// Game thread, from the overlay's rect call of the frame's last pass: the viewport of one column's UI particles
/// (hud/layout/viewport_placement.h EffectViewport: a box's centre and height as fractions of the column's image, its
/// width the target's shape), or none when the band does not apply to that column this frame. The box may reach
/// outside the image: a column whose layout is not the tick's needs the one particle moved past an edge, and the draw
/// is clipped to the target anyway.
void PublishColumnViewport(int column, const EffectBox& box, bool applies) noexcept;

/// Render thread, from the composite redirect: the column whose family's passes follow (a negative value for none).
/// Also the moment a crossing site is installed or restored.
void NoteFamilyColumn(int column) noexcept;

/// The `hud-fx-pass` switch: the bands on (the default) or off (the engine's whole-target viewport, for comparison).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

#if LSS_DEV
/// The `hud-fx-pass` command and reading, and the `hud-fx-pass.state` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::hud::ui_effect_pass
