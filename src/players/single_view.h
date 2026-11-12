#pragma once

// The engine's single view: the whole screen player 1's view alone while the seats' players are in (their controllers,
// characters and pads untouched). The one monitor outcome for the moments views/column_source.h decides (a full-screen
// menu, the map screen, a cutscene, a shared ride, every seat's player waiting for a member); this class only holds the
// engine's switch to what it is told.
//
// The engine lays the local players out from one stock switch, UGameViewportClient::
// bDisableSplitScreenOverride: while it is set the split type is None whatever the player count
// (UpdateActiveSplitscreenType, research/RE-vertical-split-and-fill.md). Its reflected setter,
// UGameplayStatics::SetForceDisableSplitscreen, lays the players out again at once. The mod sets it
// through that setter and clears it only if the mod set it and it still holds the mod's value.
//
// The engine is reached through SplitSwitchAccess, so this policy has no UE4SS dependency and is
// tested on the host. Same rules as players/split_layout: a value that cannot be read is never
// overwritten, a value someone else set is left alone, and a failure is reported once per request
// rather than retried every tick.

#include <optional>
#include <string>

namespace lss::players {

struct SplitSwitchAccess
{
    std::optional<bool> (*read)() noexcept;       // true while the split is force-disabled
    bool (*write)(bool disabled) noexcept;        // false if the setter could not be called
};

/// What an Update did, for the caller to log.
enum class SingleViewChange
{
    None,           // nothing to do
    Single,         // the mod set the switch: player 1's view alone
    AlreadySingle,  // the switch was already set by someone else; it will not be cleared by the mod
    SingleFailed,   // it could not be read, set, or confirmed; the split stays (until the request changes)
    Split,          // the mod cleared its switch: the split is back
    LeftAlone,      // the split was asked back, but the switch is not the mod's (or someone cleared it)
    SplitFailed,    // it could not be read or cleared; the screen stays single (until the request changes)
};

/// The change for the log (empty for None).
std::wstring Describe(SingleViewChange change);

class SingleView
{
public:
    explicit SingleView(SplitSwitchAccess access) noexcept : access_(access) {}

    /// Call on the game thread every tick with whether the single view is wanted (views/column_source.h decides;
    /// false once every seat is out). Acts only when the request changes (or a failed request is asked for
    /// again after a change).
    SingleViewChange Update(bool wanted) noexcept;

    /// True while the screen shows player 1's view alone through the switch (the mod's or one found
    /// set). The split's per-player presentation (the seats' HUD images, the halves' layout) is off.
    bool Single() const noexcept { return single_; }

private:
    SingleViewChange SetSingle() noexcept;
    SingleViewChange SetSplit() noexcept;

    SplitSwitchAccess access_;
    bool              single_     = false;  // the switch is set (by the mod when owned_)
    bool              owned_      = false;  // the mod set it, so the mod clears it
    bool              wanted_     = false;  // the last request
    bool              failed_     = false;  // the last request failed; not retried until it changes
};

}  // namespace lss::players
