#pragma once

// Which button art a player's device gets, by the game's own rule (research/RE-per-player-icons.md). Pure:
// host-tested (tests/button_art_test.cpp).
//
// A text block turns each `<button=NAME>` tag into art from two bytes of its font: the pad family (which pad's art) and
// the keyboard flag (keycaps). The game writes both into its three UI fonts from the last device used, through
// SquareInput_RecomputeGlyphSet: the device's platform (PlayStation, Xbox), a PlayStation pad's kind (DualShock 4 or
// DualSense), and the "button display" option (0 auto, 1 Xbox, 2 PlayStation, 3 a fourth set). The same rule here
// gives each player the art the game would show if that player's device were the last one used.

#include <cstdint>

namespace lss::hud::button_art_rules {

/// The two font bytes a layout reads.
struct Art
{
    std::uint8_t family   = 0;  // 0 DualSense, 1 Xbox, 2 DirectInput, 3 DualShock 4, 4 the fourth set
    std::uint8_t keyboard = 0;  // 1: keycaps where a tag allows them

    friend constexpr bool operator==(const Art&, const Art&) = default;
};

inline constexpr std::uint8_t kFamilyPlayStation = 0;
inline constexpr std::uint8_t kFamilyXbox        = 1;
inline constexpr std::uint8_t kFamilyDualShock4  = 3;
inline constexpr std::uint8_t kFamilyFourth      = 4;

// The "button display" option.
inline constexpr std::uint8_t kOptionAuto        = 0;
inline constexpr std::uint8_t kOptionXbox        = 1;
inline constexpr std::uint8_t kOptionPlayStation = 2;
inline constexpr std::uint8_t kOptionFourth      = 3;

// The game's per-slot pad type (Steam's answer for XInput slot k): the two PlayStation kinds, anything else Xbox-like.
inline constexpr std::uint8_t kPadTypeDualShock4 = 4;
inline constexpr std::uint8_t kPadTypeDualSense  = 5;

/// The pad family for a pad of that platform and kind under the option.
constexpr std::uint8_t PadFamily(bool playStation, bool dualShock4, std::uint8_t option) noexcept
{
    if (option == kOptionFourth)
    {
        return kFamilyFourth;
    }
    if (playStation)
    {
        return option == kOptionXbox ? kFamilyXbox : dualShock4 ? kFamilyDualShock4 : kFamilyPlayStation;
    }
    return option == kOptionPlayStation ? kFamilyPlayStation : kFamilyXbox;
}

enum class Device : std::uint8_t
{
    XInputPad,       // its slot's pad type tells a PlayStation pad behind Steam Input from an Xbox pad
    PlayStationPad,  // the game's own PlayStation reader
    Keyboard,
};

/// The art for a player on `device`: an XInput pad by its slot's type, a PlayStation pad read by the game itself (its
/// kind is not read per pad: DualSense art), the keyboard keycaps over `shared`'s family (what a `,pad` tag shows).
constexpr Art ArtOf(Device device, std::uint8_t slotType, std::uint8_t option, Art shared) noexcept
{
    switch (device)
    {
    case Device::Keyboard:
        return Art{shared.family, 1};
    case Device::PlayStationPad:
        return Art{PadFamily(true, false, option), 0};
    case Device::XInputPad:
        break;
    }
    const bool playStation = slotType == kPadTypeDualShock4 || slotType == kPadTypeDualSense;
    return Art{PadFamily(playStation, slotType == kPadTypeDualShock4, option), 0};
}

}  // namespace lss::hud::button_art_rules
