#pragma once

// Which way the screen is split for two players: top/bottom or side by side.
//
// The engine decides it every frame from one stock setting, UGameMapsSettings::
// TwoPlayerSplitscreenLayout (0 top/bottom, 1 side by side), and uses it only while there are
// exactly two local players (engine_addresses.h). So the mod does not lay anything
// out itself: it sets that setting while player 2 is in, to the layout the player chose, and puts
// the game's value back when player 2 leaves. Until a layout is chosen, the game's own is used.
//
// The engine is reached through LayoutAccess, so this policy has no UE4SS dependency and is tested
// on the host. Same rules as players/split_screen_settings: a value that cannot be read is never
// overwritten on the way out, and a value someone else set meanwhile is left alone.

#include <cstdint>
#include <optional>

namespace lss::players {

enum class SplitLayoutKind : std::uint8_t
{
    TopBottom  = 0,  // ETwoPlayerSplitScreenType::Horizontal
    SideBySide = 1,  // ETwoPlayerSplitScreenType::Vertical
};

struct LayoutAccess
{
    std::optional<std::uint8_t> (*read)() noexcept;
    bool (*write)(std::uint8_t value) noexcept;
};

/// What an Update or Toggle did, for the caller to log.
struct LayoutChange
{
    enum class Kind
    {
        None,           // nothing to do
        Chosen,         // no player 2: the choice is remembered and applied at the next join (`value`)
        Unreadable,     // a switch with nothing chosen yet, and the game's layout could not be read
        Applied,        // the setting now holds `value`
        ApplyFailed,    // it could not be read or written; the game's layout stays
        Restored,       // player 2 left: the game's value (`value`) was put back
        LeftAlone,      // player 2 left: something else changed it meanwhile (`value` = now); kept
        RestoreFailed,  // player 2 left: it could not be read or put back; retried at the next leave
    };
    Kind         kind  = Kind::None;
    std::uint8_t value = 0;
};

class SplitLayout
{
public:
    explicit SplitLayout(LayoutAccess access) noexcept : access_(access) {}

    /// Call on the game thread every tick with whether player 2 is in. Acts only when that changes.
    LayoutChange Update(bool playerTwoIn) noexcept;

    /// Switches to the other layout: now if player 2 is in, otherwise at the next join.
    LayoutChange Toggle() noexcept;

    /// Chooses a layout (a SplitLayoutKind value): now if player 2 is in, otherwise at the next join.
    LayoutChange Choose(std::uint8_t value) noexcept;

private:
    LayoutChange Apply(std::uint8_t value) noexcept;
    LayoutChange Restore() noexcept;

    LayoutAccess                access_;
    bool                        playerTwoIn_ = false;
    std::optional<std::uint8_t> chosen_;     // the player's choice; empty = the game's own
    std::optional<std::uint8_t> restoreTo_;  // the game's value, while the mod's is in place
};

}  // namespace lss::players
