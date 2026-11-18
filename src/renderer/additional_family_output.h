#pragma once

// Makes the game honour bAdditionalViewFamily again, so a later player's family keeps the earlier
// players' parts of the screen instead of clearing them.
//
// Stock Unreal clears the final output only for the first view of a family that is not marked
// additional; the shipped game clears for every family's first view (output_load_action_code.h).
// With one view per family (one_family_per_view.h), families after the first are marked
// additional, so restoring the stock rule is what lets them share the screen. One player is
// unaffected: the engine never marks the game viewport's single family additional.
// Evidence: RE-additional-view-family-reader.md.
//
// With frame generation on, the tonemap binds two more targets (the HUD-less colour and UI that DLSS-G reads) set to
// clear, and the D3D12 RHI clears every bound target when one clears: the back buffer too, whatever its own load
// action. Those two targets are set to load as well (research/RE-p2-framegen-3.md); a first family's back buffer still
// clears, which clears them with it.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

/// Installs the restored rule at the final output pass. Call once, after every engine fact has
/// been verified. Returns false, changing nothing, if it cannot be installed.
bool InstallAdditionalFamilyOutput(const platform::GameImage& image) noexcept;

/// Puts the game's own instructions back (at mod uninstall).
void RemoveAdditionalFamilyOutput() noexcept;

#if LSS_DEV
/// The `fg-targets-load` command: the frame-generation targets' store switched between the mod's and the game's, for
/// the A/B. May throw (out of memory).
void RegisterAdditionalFamilyOutputCommands();
#endif

}  // namespace lss::renderer
