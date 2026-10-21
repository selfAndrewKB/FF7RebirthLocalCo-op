#pragma once

// Player 1's single view while player 2 is in: for full-screen menus, the map screen and cutscenes
// the screen goes back to player 1's view alone (the user's decision, 2026-09-28), and player 2
// stays in (their controller, character and pad are untouched).
//
// The engine lays the local players out from one stock switch, UGameViewportClient::
// bDisableSplitScreenOverride: while it is set the split type is None whatever the player count
// (UpdateActiveSplitscreenType, RE-vertical-split-and-fill.md §Q1). Its reflected setter,
// UGameplayStatics::SetForceDisableSplitscreen, lays the players out again at once. The mod sets it
// through that setter and clears it only if the mod set it and it still holds the mod's value.
//
// The engine is reached through SplitSwitchAccess, so this policy has no UE4SS dependency and is
// tested on the host. Same rules as players/split_layout: a value that cannot be read is never
// overwritten, a value someone else set is left alone, and a failure is reported once per request
// rather than retried every tick.

#include <cstdint>
#include <optional>
#include <string>

namespace lss::players {

struct SplitSwitchAccess
{
    std::optional<bool> (*read)() noexcept;       // true while the split is force-disabled
    bool (*write)(bool disabled) noexcept;        // false if the setter could not be called
};

/// Why the screen should be single, decided by the caller from the game's own state.
enum class SingleViewMode : std::uint8_t
{
    Auto,    // single while the game shows something that takes the whole screen (the default)
    Single,  // always single while player 2 is in (development: the command channel)
    Split,   // never single (development)
};

/// What an Update or SetMode did, for the caller to log.
struct SingleViewChange
{
    enum class Kind
    {
        None,           // nothing to do
        Single,         // the mod set the switch: player 1's view alone
        AlreadySingle,  // the switch was already set by someone else; it will not be cleared by the mod
        SingleFailed,   // it could not be read, set, or confirmed; the split stays (until the request changes)
        Split,          // the mod cleared its switch: the split is back
        LeftAlone,      // the split was asked back, but the switch is not the mod's (or someone cleared it)
        SplitFailed,    // it could not be read or cleared; the screen stays single (until the request changes)
    };
    Kind kind = Kind::None;
};

/// The screen is single while the automatic choice is on and the game shows something that takes the whole
/// screen, or player 2 waits for a party member, or player 2 stands aside for a shared ride (each already known
/// only while player 2 is in).
constexpr bool SingleViewWanted(bool autoMode, bool wholeScreenShown, bool waitsForMember, bool standsAside) noexcept
{
    return autoMode && (wholeScreenShown || waitsForMember || standsAside);
}

/// The change for the log (empty for None).
std::wstring Describe(const SingleViewChange& change);

class SingleView
{
public:
    explicit SingleView(SplitSwitchAccess access) noexcept : access_(access) {}

    /// Call on the game thread every tick: whether player 2 is in, and whether the game now shows
    /// something that takes the whole screen. Acts only when the resulting request changes (or a
    /// failed request is asked for again after a change).
    SingleViewChange Update(bool playerTwoIn, bool wholeScreenShown) noexcept;

    /// Development override of the automatic choice; applied at the next Update.
    void SetMode(SingleViewMode mode) noexcept { mode_ = mode; }
    SingleViewMode Mode() const noexcept { return mode_; }

    /// True while the screen shows player 1's view alone through the switch (the mod's or one found
    /// set). The split's per-player presentation (player 2's HUD image, the halves' layout) is off.
    bool Single() const noexcept { return single_; }

private:
    SingleViewChange SetSingle() noexcept;
    SingleViewChange SetSplit() noexcept;

    SplitSwitchAccess access_;
    SingleViewMode    mode_       = SingleViewMode::Auto;
    bool              single_     = false;  // the switch is set (by the mod when owned_)
    bool              owned_      = false;  // the mod set it, so the mod clears it
    bool              wanted_     = false;  // the last request
    bool              failed_     = false;  // the last request failed; not retried until it changes
};

}  // namespace lss::players
