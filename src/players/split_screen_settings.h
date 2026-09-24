#pragma once

// Engine settings two-player rendering needs, set while player 2 is in and put back when they leave.
//
// r.SceneRenderTargetResizeMethod (a stock engine setting): each player's family asks for scene
// textures sized to its own part of the screen, and at the default (0, "resize to match requested
// render size") the engine re-creates them for every family, twice a frame. Measured: split frames
// 69-84 ms at 0, 14.8-16.4 ms at 2 ("expands to encompass the largest requested render
// dimension"), back to the stall at 0 again (endstate §11.124). With one player the largest request
// is the full screen the game already uses, so 2 costs no memory there; it is still only set while
// player 2 is in, so one-player play keeps the game's own value.
//
// The engine is reached through ConsoleAccess, so this policy has no UE4SS dependency and is
// tested on the host.

#include <cstdint>
#include <optional>

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

class SplitScreenSettings
{
public:
    static constexpr const wchar_t* kResizeMethod        = L"r.SceneRenderTargetResizeMethod";
    static constexpr std::int32_t   kGrowToLargestRequest = 2;

    explicit SplitScreenSettings(ConsoleAccess console) noexcept : console_(console) {}

    /// Call on the game thread every tick with whether player 2 is in. Acts only when that changes.
    SettingsChange Update(bool playerTwoIn) noexcept;

private:
    SettingsChange Apply() noexcept;
    SettingsChange Restore() noexcept;
    bool           Execute(std::int32_t value) noexcept;  // true if the console command ran

    ConsoleAccess               console_;
    bool                        playerTwoIn_ = false;
    std::optional<std::int32_t> restoreTo_;  // set once this object has run a command, until settled
};

}  // namespace lss::players
