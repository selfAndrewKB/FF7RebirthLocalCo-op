#pragma once

// Engine settings two-player rendering needs, held while player 2 is in and put back when they leave (each one a
// players::HeldSetting over the console; mod.cpp holds them).
//
// r.SceneRenderTargetResizeMethod (a stock engine setting): each player's family asks for scene
// textures sized to its own part of the screen, and at the default (0, "resize to match requested
// render size") the engine re-creates them for every family, twice a frame, which stalls the
// frame (roughly 70-85 ms against 15 ms). At 2 ("expands to encompass the largest requested render
// dimension") they are created once and shared. With one player the largest request is the full
// screen the game already uses, so 2 costs no memory there; it is still only set while player 2 is
// in, so one-player play keeps the game's own value. If it cannot be set, rendering stays correct but slower.
//
// D3D12.EnableGapRecorder 0 (performance, RE-perf-submission-path.md; measured 2026-10-08: -0.31 ms per frame in split):
// the gap recorder's per-command-list timestamps, resolves and two extra submissions per frame only feed
// GGPUFrameTime, which only stock dynamic resolution (when its timing method is 0), `stat unit` and the FPS charts read
// (RE-perf-me-shadows.md Phase 4). So r.DynamicRes.GPUTimingMeasureMethod is set to 1 (timestamp queries) first,
// and dynamic resolution, if the player turned it on, keeps real GPU timings. Each setting is one object; the gap
// recorder's goes off after the timing method's and comes back before it (SettingsOrder).

#include <array>
#include <cstdint>

namespace lss::players {

/// One console setting and the value held while player 2 is in.
struct ConsoleSetting
{
    const wchar_t* name;
    std::int32_t   value;
};

/// The three settings, indexed by SettingsStep.
enum class SettingsStep
{
    Resolution,
    Timing,
    Gap,
};
constexpr std::array<ConsoleSetting, 3> kSplitScreenSettings{{
    {L"r.SceneRenderTargetResizeMethod", 2},     // grow to the largest request
    {L"r.DynamicRes.GPUTimingMeasureMethod", 1},  // timestamp queries
    {L"D3D12.EnableGapRecorder", 0},
}};

/// The order they are updated in: joining, the timing method before the gap recorder goes off; leaving, the gap
/// recorder back before the timing method.
constexpr std::array<SettingsStep, 3> SettingsOrder(bool playerTwoIn) noexcept
{
    return playerTwoIn ? std::array<SettingsStep, 3>{SettingsStep::Resolution, SettingsStep::Timing, SettingsStep::Gap}
                       : std::array<SettingsStep, 3>{SettingsStep::Resolution, SettingsStep::Gap, SettingsStep::Timing};
}

/// The setting of a step.
constexpr const ConsoleSetting& SettingOf(SettingsStep step) noexcept
{
    return kSplitScreenSettings[static_cast<std::size_t>(step)];
}

}  // namespace lss::players
