#pragma once

// Submits a multi-view family to the renderer as one family per view.
//
// Why: the FF7R "ME" meshlet renderer was written for one view per family. When a family holds
// two views, the second view's work is not bounded to its own rectangle (its tile builder
// scans the first view's rows, which can crash the GPU), and the views share scratch state
// inside one render pass. With one view per family the engine places every view at the origin
// of the scene targets and renders it alone, so all of that is correct in stock code.
//
// This file is pure logic over the family's memory, with no engine or UE4SS dependency, so it
// is tested on the host (tests/one_family_per_view_test.cpp).

#include <cstddef>

namespace lss::renderer {

/// The engine's BeginRenderingViewFamily(this, FCanvas*, FSceneViewFamily*).
using SubmitViewFamilyFn = void (*)(void* rendererModule, void* canvas, std::byte* viewFamily);

/// Submits `viewFamily` once per view, each time presenting it with only that view.
///
/// - One view (or none): a single ordinary submission; nothing is changed.
/// - N views: view k is swapped into slot 0 and the view count is set to 1 for submission k.
///   Submissions after the first also set the frame-number skip (+0x70) and
///   bAdditionalViewFamily (+0x74), so the frame number steps once per frame and each
///   submission keeps the output of the ones before it.
///
/// The family is restored exactly before returning: the same view order, count and flags.
/// Draw deletes its views afterwards through this list, so an unrestored family would corrupt
/// the engine's heap.
///
/// Reusing one family object is safe because the renderer copies the family and its views when
/// a submission starts (stock SceneRendering.cpp:2137, 2167-2169). The views are swapped within
/// their own array, never replaced by another buffer, so even an engine that grew the array
/// during a submission would reallocate memory it owns.
void SubmitOneFamilyPerView(void* rendererModule, void* canvas, std::byte* viewFamily,
                            SubmitViewFamilyFn engineSubmit) noexcept;

}  // namespace lss::renderer
