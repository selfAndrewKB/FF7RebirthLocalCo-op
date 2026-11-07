#pragma once

// Which way the screen is split for two players: top/bottom or side by side.
//
// The engine decides it every frame from one stock setting, UGameMapsSettings::
// TwoPlayerSplitscreenLayout (0 top/bottom, 1 side by side), and uses it only while there are
// exactly two local players (engine_addresses.h). So the mod does not lay anything
// out itself: it holds that setting while there are two local players, at the layout the player chose,
// and puts the game's value back when that ends (players::HeldSetting). The choice itself is this module's: the
// settings file's word at start (platform/settings.h kTwoPlayerSplit, applied by ChooseFromSettings), then Ctrl+F5 or
// the console for the session; it is remembered across leaves and applied at each join.

#include "platform/dev_switch.h"
#include "players/held_setting.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lss::players {

enum class SplitLayoutKind : std::uint8_t
{
    TopBottom  = 0,  // ETwoPlayerSplitScreenType::Horizontal
    SideBySide = 1,  // ETwoPlayerSplitScreenType::Vertical
};

/// A layout value's text for the log: "1 (side by side)".
std::wstring LayoutText(std::int32_t value);

/// The layout the option's word names (the engine's own words: vertical = side by side, horizontal = top/bottom);
/// top/bottom for any other text.
SplitLayoutKind LayoutOfWord(std::wstring_view word) noexcept;

/// The option's word for a layout value: "vertical" or "horizontal".
const wchar_t* WordOfLayout(std::uint8_t value) noexcept;

class SplitLayout
{
public:
    static constexpr const wchar_t* kName = L"TwoPlayerSplitscreenLayout";

    explicit SplitLayout(SettingAccess<std::uint8_t> access) noexcept : held_(access, kName) {}

    /// Call on the game thread every tick with whether two columns are listed. Acts only when that changes.
    HeldChange Update(bool twoColumns) noexcept;

    /// Switches to the other layout: now if two columns are listed, otherwise at the next join.
    HeldChange Toggle() noexcept;

    /// Chooses a layout (a SplitLayoutKind value): now if two columns are listed, otherwise at the next join.
    HeldChange Choose(std::uint8_t value) noexcept;

    /// The player's choice (a SplitLayoutKind value), or empty while the game's own layout is used.
    std::optional<std::uint8_t> Chosen() const noexcept { return chosen_; }

    /// The layout in force: the choice, else the game's setting as read now (empty if it cannot be read).
    std::optional<std::uint8_t> Current() const noexcept { return chosen_ ? chosen_ : held_.Read(); }

private:
    HeldSetting<std::uint8_t>   held_;
    std::optional<std::uint8_t> chosen_;  // the player's choice; empty = the game's own
};

/// The change for the log (empty for None).
inline std::wstring Describe(const SplitLayout&, const HeldChange& change)
{
    return Describe(change, L"split layout", SplitLayout::kName, &LayoutText);
}

/// Chooses the layout the settings file reads (after settings::Load, before any join) and logs it with its source.
void ChooseFromSettings(SplitLayout& layout) noexcept;

#if LSS_DEV
/// The `two-player-split` command and reading from the option's row (vertical, horizontal: the layout in force, chosen
/// or the game's; empty if the game's setting cannot be read). May throw (out of memory); modules::RegisterCommands
/// catches.
void RegisterCommands(SplitLayout& layout);
#endif

}  // namespace lss::players
