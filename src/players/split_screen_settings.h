#pragma once

// Engine settings two-player rendering needs, set while player 2 is in and put back when they leave.
//
// r.SceneRenderTargetResizeMethod (a stock engine setting): each player's family asks for scene
// textures sized to its own part of the screen, and at the default (0, "resize to match requested
// render size") the engine re-creates them for every family, twice a frame, which stalls the
// frame (roughly 70-85 ms against 15 ms). At 2 ("expands to encompass the largest requested render
// dimension") they are created once and shared. With one player the largest request is the full
// screen the game already uses, so 2 costs no memory there; it is still only set while player 2 is
// in, so one-player play keeps the game's own value.
//
// D3D12.EnableGapRecorder 0 (performance, RE-perf-submission-path.md; measured 2026-10-08: -0.31 ms per frame in split):
// the gap recorder's per-command-list timestamps, resolves and two extra submissions per frame only feed
// GGPUFrameTime, which only stock dynamic resolution (when its timing method is 0), `stat unit` and the FPS charts read
// (RE-perf-me-shadows.md Phase 4). So r.DynamicRes.GPUTimingMeasureMethod is set to 1 (timestamp queries) first,
// and dynamic resolution, if the player turned it on, keeps real GPU timings. Each setting is one object; the gap
// recorder's goes off after the timing method's and comes back before it.
//
// The engine is reached through ConsoleAccess, so this policy has no UE4SS dependency and is
// tested on the host.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace lss::players {

struct ConsoleAccess
{
    std::optional<std::int32_t> (*readInt)(const wchar_t* variable) noexcept;
    bool (*execute)(const wchar_t* command) noexcept;
};

/// What an Update did, for the caller to log.
struct SettingsChange
{
    enum class Kind
    {
        None,           // nothing to do this tick
        Applied,        // player 2 joined: the setting was changed (from `value`)
        AlreadySet,     // player 2 joined: it already had the needed value; nothing to restore later
        ApplyFailed,    // player 2 joined: it could not be read, set, or confirmed (runs correctly, maybe slowly)
        Restored,       // player 2 left: the previous value (`value`) was put back
        LeftAlone,      // player 2 left: something else changed it meanwhile (`value` = now); kept
        RestoreFailed,  // player 2 left: it could not be read or put back; retried at the next leave
    };
    Kind         kind  = Kind::None;
    std::int32_t value = 0;
};

/// The three settings the mod holds, in the order they are updated: joining, the timing method before the gap
/// recorder goes off; leaving, the gap recorder back before the timing method.
enum class SettingsStep
{
    Resolution,
    Timing,
    Gap,
};
constexpr std::array<SettingsStep, 3> SettingsOrder(bool playerTwoIn) noexcept
{
    return playerTwoIn ? std::array<SettingsStep, 3>{SettingsStep::Resolution, SettingsStep::Timing, SettingsStep::Gap}
                       : std::array<SettingsStep, 3>{SettingsStep::Resolution, SettingsStep::Gap, SettingsStep::Timing};
}

class SplitScreenSettings;

/// The change for the log (empty for None).
std::wstring Describe(const SplitScreenSettings& setting, const SettingsChange& change);

class SplitScreenSettings
{
public:
    static constexpr const wchar_t* kResizeMethod        = L"r.SceneRenderTargetResizeMethod";
    static constexpr std::int32_t   kGrowToLargestRequest = 2;
    static constexpr const wchar_t* kTimingMethod        = L"r.DynamicRes.GPUTimingMeasureMethod";
    static constexpr std::int32_t   kTimestampQueries    = 1;
    static constexpr const wchar_t* kGapRecorder         = L"D3D12.EnableGapRecorder";
    static constexpr std::int32_t   kGapRecorderOff      = 0;

    /// One integer setting, `name`, held at `value` while player 2 is in (`name` must outlive the object).
    explicit SplitScreenSettings(ConsoleAccess console, const wchar_t* name = kResizeMethod,
                                 std::int32_t value = kGrowToLargestRequest) noexcept
        : console_(console), name_(name), value_(value)
    {
    }

    /// Call on the game thread every tick with whether player 2 is in. Acts only when that changes.
    SettingsChange Update(bool playerTwoIn) noexcept;

    const wchar_t* Name() const noexcept { return name_; }
    std::int32_t   Value() const noexcept { return value_; }

private:
    SettingsChange Apply() noexcept;
    SettingsChange Restore() noexcept;
    bool           Execute(std::int32_t value) noexcept;  // true if the console command ran

    ConsoleAccess               console_;
    const wchar_t*              name_;
    std::int32_t                value_;
    bool                        playerTwoIn_ = false;
    std::optional<std::int32_t> restoreTo_;  // set once this object has run a command, until settled
};

}  // namespace lss::players
