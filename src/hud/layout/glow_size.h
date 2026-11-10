#pragma once

// The summon gauge glow's size in a split view. The aura behind the bar is a Niagara system
// (FX_R_Menu_SummonGauge_Aura_01) whose sprite sizes are constants baked into its scripts: no user parameter, the
// owner's scale never read (research/RE-summon-gauge.md §7.5, §7.6). The effect is drawn against the whole UI image,
// so its sprites keep their single-player size while the bar is drawn at the band's scale: in a split the aura stands
// at about twice its bar. This unit scales those constants in the loaded scripts by the band's drawing scale over
// single player's (hud/layout/viewport_placement.h EffectSizeFactor): each emitter's size literal in its spawn script's
// VM literal table, a table of known indices and values that is checked against the loaded asset before any write.
// The VM reads the table when an aura spawns, so a written size shows from the next aura on (the next fight, or the
// gauge's next appearance), and the asset's values are written back when the split ends (a factor of 1). Every field
// is reached through the engine's reflection (the Niagara classes' properties by name), resolved once and checked for
// size; nothing is written unless every name resolves. The system object is looked up by its asset path when needed
// (loaded with the battle HUD) and dropped when its slot no longer holds it.

#include "platform/dev_switch.h"

namespace lss::hud {

/// Game thread, every tick: the factor the aura's sizes need now (1 while not split, or the layout unknown). Quiet
/// while it stands as last written; a change writes every known constant and logs it.
void FollowGlowSize(float factor) noexcept;

/// The asset's own values written back at once (at the mod's uninstall, while the game goes on without it).
void ResetGlowSize() noexcept;

#if LSS_DEV
/// `glow-size-dump` (every parameter store and literal table of the aura's scripts to the log), `glow-size` (a factor
/// instead of the layout's, or `auto`) and `glow-literal` (one literal of one script set, to find the loop emitters'
/// sizes). May throw (out of memory).
void RegisterGlowSizeCommands();
#endif

}  // namespace lss::hud
