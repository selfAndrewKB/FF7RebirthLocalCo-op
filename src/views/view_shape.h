#pragma once

// How a split player's view fills its part of the screen: the arithmetic on the FMinimalViewInfo
// (views/minimal_view_info.h) and the FOV rule's values, kept apart from the engine so they are tested on the host.
//
// The game's cameras ask for 16:9 with bConstrainAspectRatio set, so the engine keeps each split view 16:9 and fills
// the rest with bars. With the flag cleared on each main camera (through the game's setter,
// camera/main_camera_shape.h) the view takes its whole part of the window; the field of view then follows
// ULocalPlayer::AspectRatioAxisConstraint, which the mod sets (MajorAxisFOV, or MaintainXFOV with the split FOV of
// views/split_fov_rules.h written in). RE-vertical-split-and-fill.md.

#include "platform/settings.h"
#include "views/split_fov_rules.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lss::views {

/// EAspectRatioAxisConstraint::AspectRatio_MajorAxisFOV: a view taller than wide keeps the vertical FOV, a view wider
/// than tall the horizontal FOV.
inline constexpr std::uint8_t kMajorAxisFov = 2;

/// EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV: the FOV field is the horizontal FOV of the
/// view as drawn. The rule the split FOV writes its result for.
inline constexpr std::uint8_t kMaintainXFov = 1;

/// The weight a settings word stands for ("0.35": platform/settings.h kHorizontalFov's words, steps of 0.05), by the
/// word's place in the table, so no locale's decimal mark is read; `fallback` (the option's default) for any other text.
inline float SplitFovWeightOf(std::wstring_view word, float fallback = split_fov_rules::kDefaultWeight) noexcept
{
    const auto step = platform::settings::WordIndex(platform::settings::kHorizontalFov, word);
    return step ? static_cast<float>(*step * 5) / 100.0f : fallback;
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

/// The width of a split view against the camera's single-player view, as the ratio of the tangents of the two
/// horizontal FOVs (degrees); 1 for an unusable result.
float WidthRatio(float splitFovDegrees, float cameraFovDegrees) noexcept;

/// The vertical FOV (degrees) of a view `horizontalFovDegrees` wide at `aspect`.
float VerticalFovDegrees(float horizontalFovDegrees, float aspect) noexcept;

/// How often the viewport's size is read while the split shows (it changes with the window): twice a second at 60
/// ticks. Each view's own shape is its fractions of that size, read at each view point.
inline constexpr std::uint32_t kViewportSizeIntervalTicks = 30;

/// Keeps the viewport-size reads paced and the size's log on change: reset while the split is not shown, a read every
/// kViewportSizeIntervalTicks ticks otherwise.
class ViewportSizeWatch
{
public:
    enum class Step
    {
        Reset,  // the split is not shown: clear the size
        Skip,   // not this tick
        Read,   // read the viewport
    };

    Step Next(bool splitShown) noexcept
    {
        if (!splitShown)
        {
            ticks_        = 0;
            loggedWidth_  = -1;
            loggedHeight_ = -1;
            return Step::Reset;
        }
        return ticks_++ % kViewportSizeIntervalTicks != 0 ? Step::Skip : Step::Read;
    }

    /// True once per change of the size read (the caller logs it).
    bool SizeChanged(std::int32_t width, std::int32_t height) noexcept
    {
        if (width == loggedWidth_ && height == loggedHeight_)
        {
            return false;
        }
        loggedWidth_  = width;
        loggedHeight_ = height;
        return true;
    }

private:
    std::uint32_t ticks_        = 0;
    std::int32_t  loggedWidth_  = -1;
    std::int32_t  loggedHeight_ = -1;
};

/// True if the struct holds an aspect ratio and a FOV a real camera could have, as this build's
/// FMinimalViewInfo does at those offsets. A struct that fails is not written.
bool LooksLikeViewInfo(const std::byte* viewInfo) noexcept;

}  // namespace lss::views
