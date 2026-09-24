#pragma once

// Routes the game viewport's view family through SubmitOneFamilyPerView.
//
// The mod replaces the IRendererModule vtable entry for BeginRenderingViewFamily. Every caller
// reaches the renderer through that entry; only the game viewport's Draw (identified by its
// return address) is split, and only when its family holds more than one view. Every other
// submission, and every submission with one player, goes to the engine unchanged.

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

/// Installs the submission. Call once, after every engine fact has been verified.
/// Returns false, changing nothing, if the vtable entry does not hold the engine's function.
bool InstallViewFamilySubmission(const platform::GameImage& image) noexcept;

/// Puts the engine's own function back in the entry (at mod uninstall).
void RemoveViewFamilySubmission() noexcept;

}  // namespace lss::renderer
