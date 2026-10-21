#pragma once

// The frame watch's hitch rule (platform/frame_watch.h) as a pure step, tested on the host (tests/hitch_step_test.cpp).

namespace lss::platform {

struct HitchState
{
    double   averageMs = 0.0;  // the running average frame time (0: no frame seen yet)
    unsigned run       = 0;    // hitch frames in a row
};

enum class FrameKind
{
    First,      // the first frame: it becomes the average
    Hitch,      // over the average by the ratio and the margin; the average is kept (it would hide the next one)
    NewNormal,  // more hitch frames in a row than the run allows: the frame becomes the average (a load ended, say)
    Steady,     // the average moves toward the frame by the weight
};

constexpr FrameKind HitchStep(HitchState& state, double frameMs, double hitchRatio, double hitchMarginMs,
                              unsigned newNormalRun, double averageWeight) noexcept
{
    if (state.averageMs == 0.0)
    {
        state.averageMs = frameMs;
        return FrameKind::First;
    }
    if (frameMs > state.averageMs * hitchRatio && frameMs > state.averageMs + hitchMarginMs && ++state.run <= newNormalRun)
    {
        return FrameKind::Hitch;
    }
    if (state.run > newNormalRun)
    {
        state.averageMs = frameMs;
        state.run       = 0;
        return FrameKind::NewNormal;
    }
    state.run = 0;
    state.averageMs += (frameMs - state.averageMs) * averageWeight;
    return FrameKind::Steady;
}

}  // namespace lss::platform
