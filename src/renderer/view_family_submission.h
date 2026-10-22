#pragma once

// Routes the game viewport's view family through SubmitOneFamilyPerView.
//
// The mod replaces the IRendererModule vtable entry for BeginRenderingViewFamily. Every caller
// reaches the renderer through that entry; only the game viewport's Draw (identified by its
// return address) is split, and only when its family holds more than one view. Every other
// submission, and every submission with one player, goes to the engine unchanged.
//
// With the perf log on, the renderer's report at the end of each 10 s window (platform/frame_watch.h) gives the
// mean frame time, the view count and each family's time inside the engine's submission.

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

/// Installs the submission. Call once, after every engine fact has been verified.
/// Returns false, changing nothing, if the vtable entry does not hold the engine's function.
bool InstallViewFamilySubmission(const platform::GameImage& image) noexcept;

/// Diagnosis (family-split): true, one family per view (at start); false, the engine's one multi-view family.
void SetFamilySplit(bool onePerView) noexcept;

/// Puts the engine's own function back in the entry (at mod uninstall).
void RemoveViewFamilySubmission() noexcept;

}  // namespace lss::renderer
