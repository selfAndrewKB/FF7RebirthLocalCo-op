#pragma once

// How a split player's view fills its part of the screen: the arithmetic on the FMinimalViewInfo
// (views/minimal_view_info.h), kept apart from the engine so it is tested on the host.
//
// The game's cameras ask for 16:9 with bConstrainAspectRatio set, so the engine keeps each split
// half 16:9 and fills the rest with bars. With the flag cleared on each main camera (through the
// game's setter, camera/main_camera_shape.h) the view takes its whole half; the field of view
// then follows ULocalPlayer::AspectRatioAxisConstraint, which the mod sets (MajorAxisFOV, or
// MaintainXFOV with the split FOV below). RE-vertical-split-and-fill.md.

#include "platform/settings.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lss::views {

/// EAspectRatioAxisConstraint::AspectRatio_MajorAxisFOV: a side-by-side half (taller than wide)
/// keeps the vertical FOV, a top/bottom half (wider than tall) keeps the horizontal FOV.
inline constexpr std::uint8_t kMajorAxisFov = 2;

/// EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV: the FOV field is the horizontal FOV of the
/// view as drawn. The rule the split FOV blend writes its result for.
inline constexpr std::uint8_t kMaintainXFov = 1;

/// One player's half of a `width` x `height` viewport split in two: its width/height, and its
/// height as a fraction of the viewport's (1 side by side, 0.5 top/bottom). Zeroes if unusable.
struct HalfShape
{
    float aspect      = 0.0f;
    float heightRatio = 0.0f;
};
HalfShape HalfOf(std::int32_t width, std::int32_t height, bool sideBySide) noexcept;

/// The weight the split FOV gives to keeping the characters' on-screen size over keeping the
/// single-player framing (SplitFov); 0.5 is the geometric middle, chosen by eye.
inline constexpr float kDefaultSplitFovWeight = 0.5f;

/// The weight a settings word stands for ("0.35": platform/settings.h kHorizontalFov's words, steps of 0.05), by the
/// word's place in the table, so no locale's decimal mark is read; kDefaultSplitFovWeight for any other text.
inline float SplitFovWeightOf(std::wstring_view word) noexcept
{
    const auto step = platform::settings::WordIndex(platform::settings::kHorizontalFov, word);
    return step ? static_cast<float>(*step * 5) / 100.0f : kDefaultSplitFovWeight;
}

/// The option's word for a weight from 0 to 1 (the nearest step: "0.50"); empty for a weight outside that (the
/// MajorAxisFOV rule), which is no word of the option's.
inline std::wstring SplitFovWordOf(float weight)
{
    if (!(weight >= 0.0f && weight <= 1.0f))
    {
        return {};
    }
    const auto step = static_cast<std::size_t>(weight * 20.0f + 0.5f);
    return platform::settings::kFovWeightWords[step < platform::settings::kFovWeightSteps ? step
                                                                                          : platform::settings::kFovWeightSteps - 1];
}

/// The horizontal FOV (degrees) for a split half, from the camera's single-player view
/// (`fovDegrees` wide at `cameraAspect`, the 16:9 the game's cameras ask for). A split view can
/// keep the single-player *framing* (the same vertical FOV, so a character sits at the same
/// fraction of the height) or the single-player *scale* (the same pixels per degree, so a
/// character is as many pixels tall, which needs a vertical FOV in proportion to the half's
/// height). Side by side, a half is as tall as the screen and the two agree. Top/bottom they
/// conflict, and `weight` picks between them:
///     tan(vertical/2) = tan(solo vertical/2) * heightRatio^weight
/// (0: framing kept; 1: scale kept). Returns `fovDegrees` unchanged for unusable inputs.
float SplitFov(float fovDegrees, float cameraAspect, const HalfShape& half, float weight) noexcept;

/// The width of a split view against the camera's single-player view, as the ratio of the tangents of the two
/// horizontal FOVs (degrees); 1 for an unusable result.
float WidthRatio(float splitFovDegrees, float cameraFovDegrees) noexcept;

/// The vertical FOV (degrees) of a view `horizontalFovDegrees` wide at `aspect`.
float VerticalFovDegrees(float horizontalFovDegrees, float aspect) noexcept;

/// How often the halves' shape is read while the split shows (the viewport size changes with the window, the
/// layout by key or script): twice a second at 60 ticks.
inline constexpr std::uint32_t kHalfShapeIntervalTicks = 30;

/// Keeps the half-shape reads paced and the aspect's log on change: reset while the split is not shown or three
/// columns are (the halves rule does not apply), a read every kHalfShapeIntervalTicks ticks otherwise.
class HalfShapeWatch
{
public:
    enum class Step
    {
        Reset,  // the halves rule does not apply: clear the shape
        Skip,   // not this tick
        Read,   // read the viewport and the layout
    };

    Step Next(bool splitShown, bool threeColumns) noexcept
    {
        if (!splitShown || threeColumns)
        {
            ticks_        = 0;
            loggedAspect_ = -1.0f;
            return Step::Reset;
        }
        return ticks_++ % kHalfShapeIntervalTicks != 0 ? Step::Skip : Step::Read;
    }

    /// True once per change of the aspect read (the caller logs it).
    bool AspectChanged(float aspect) noexcept
    {
        if (aspect == loggedAspect_)
        {
            return false;
        }
        loggedAspect_ = aspect;
        return true;
    }

private:
    std::uint32_t ticks_        = 0;
    float         loggedAspect_ = -1.0f;
};

/// True if the struct holds an aspect ratio and a FOV a real camera could have, as this build's
/// FMinimalViewInfo does at those offsets. A struct that fails is not written.
bool LooksLikeViewInfo(const std::byte* viewInfo) noexcept;

}  // namespace lss::views
