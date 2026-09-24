#pragma once

// Makes the game honour bAdditionalViewFamily again, so a later player's family keeps the earlier
// players' parts of the screen instead of clearing them.
//
// Stock Unreal clears the final output only for the first view of a family that is not marked
// additional; the shipped game clears for every family's first view (output_load_action_code.h).
// With one view per family (one_family_per_view.h), families after the first are marked
// additional, so restoring the stock rule is what lets them share the screen. One player is
// unaffected: the engine never marks the game viewport's single family additional.
// RE-additional-view-family-reader.md; SPLITSCREEN-ARCHITECTURE.md §8.

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

/// Installs the restored rule at the final output pass. Call once, after every engine fact has
/// been verified. Returns false, changing nothing, if it cannot be installed.
bool InstallAdditionalFamilyOutput(const platform::GameImage& image) noexcept;

/// Puts the game's own instructions back (at mod uninstall).
void RemoveAdditionalFamilyOutput() noexcept;

}  // namespace lss::renderer
