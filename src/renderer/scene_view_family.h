#pragma once

// The few FSceneViewFamily and FSceneView fields the mod reads or writes, with the evidence for
// each. Everything else about these engine structs is left alone.
//
// Layout: RE-onefamily-feasibility.md §Q1 (the FSceneViewFamily table), read from the FF7R
// constructor 0x140CC42D0 and matched to stock SceneView.h field order.

#include <cstddef>

namespace lss::renderer::family {

/// `TArray<const FSceneView*> Views`: the data pointer, the count and the capacity.
/// [CONFIRMED: constructor zeroes +0x00/+0x08; Draw reads Views.Num() at +0x08]
inline constexpr std::size_t kViewsData  = 0x00;
inline constexpr std::size_t kViewsCount = 0x08;

/// SE-added field: when nonzero, BeginRenderingViewFamily does not advance the scene's frame
/// number. [CONFIRMED by the instruction that reads it: engine::kFrameNumberGate]
/// Set on every family after the first in a frame, so the frame number steps once per frame,
/// as it does with one family (motion vectors and per-frame histories depend on this).
inline constexpr std::size_t kSkipFrameNumberIncrement = 0x70;

/// `bool bAdditionalViewFamily` (a whole byte). When set, the final output loads the render
/// target instead of clearing it, so an earlier family's output in another part of the screen
/// survives. The shipped game never reads it; renderer/additional_family_output.h restores the
/// stock reader (ScreenPass.inl:87), which reads the scene renderer's copy (copied at 0x140BEE082).
/// [CONFIRMED position: engine::kAdditionalFamilyStore; RE-additional-view-family-reader.md]
inline constexpr std::size_t kAdditionalViewFamily = 0x74;

}  // namespace lss::renderer::family

namespace lss::renderer::view {

/// `FIntRect UnscaledViewRect` of FSceneView: the view's area of the final output, in pixels
/// (Min.X, Min.Y, Max.X, Max.Y). Read only, for the log.
/// [CONFIRMED: FSceneView constructor 0x140CCD22A — RE-multiview-rect-sources.md]
inline constexpr std::size_t kUnscaledViewRect = 0x238;

}  // namespace lss::renderer::view
