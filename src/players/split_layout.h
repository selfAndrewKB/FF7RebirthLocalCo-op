#pragma once

// Which way the screen is split for two players: top/bottom or side by side.
//
// The engine decides it every frame from one stock setting, UGameMapsSettings::
// TwoPlayerSplitscreenLayout (0 top/bottom, 1 side by side), and uses it only while there are
// exactly two local players (engine_addresses.h). So the mod does not lay anything
// out itself: it holds that setting while player 2 is in, at the layout the player chose, and puts
// the game's value back when player 2 leaves (players::HeldSetting). Until a layout is chosen, the game's own is
// used. The choice itself is this module's: it is remembered across leaves and applied at each join.

#include "players/held_setting.h"

#include <cstdint>
#include <optional>
#include <string>

namespace lss::players {

enum class SplitLayoutKind : std::uint8_t
{
    TopBottom  = 0,  // ETwoPlayerSplitScreenType::Horizontal
    SideBySide = 1,  // ETwoPlayerSplitScreenType::Vertical
};

/// A layout value's text for the log: "1 (side by side)".
std::wstring LayoutText(std::int32_t value);

class SplitLayout
{
public:
    static constexpr const wchar_t* kName = L"TwoPlayerSplitscreenLayout";

    explicit SplitLayout(SettingAccess<std::uint8_t> access) noexcept : held_(access, kName) {}

    /// Call on the game thread every tick with whether player 2 is in. Acts only when that changes.
    HeldChange Update(bool playerTwoIn) noexcept;

    /// Switches to the other layout: now if player 2 is in, otherwise at the next join.
    HeldChange Toggle() noexcept;

    /// Chooses a layout (a SplitLayoutKind value): now if player 2 is in, otherwise at the next join.
    HeldChange Choose(std::uint8_t value) noexcept;

private:
    HeldSetting<std::uint8_t>   held_;
    std::optional<std::uint8_t> chosen_;  // the player's choice; empty = the game's own
};

/// The change for the log (empty for None).
inline std::wstring Describe(const SplitLayout&, const HeldChange& change)
{
    return Describe(change, L"split layout", SplitLayout::kName, &LayoutText);
}

}  // namespace lss::players
