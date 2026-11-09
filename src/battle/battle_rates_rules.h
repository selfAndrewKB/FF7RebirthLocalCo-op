#pragma once

// The battle's rates (battle/battle_rates.h): the multipliers' words and the arithmetic, done the game's way. Pure:
// tested on the host (tests/battle_rates_rules_test.cpp).
//
// A multiplier is held as tenths (1 to 100: 0.1 to 10.0), the settings table's words (platform/settings.h). A scaled
// value is rounded as the game rounds its own rates (research/RE-battle-rates.md): `2 * v * m + 0.5` in float, converted
// rounding to nearest, then halved (a half rounds up), at least 1 for a positive value. The AP bonus is the game's integer percent, whole at every tenth.

#include "platform/settings.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lss::battle::battle_rates_rules {

inline constexpr int kOne = 10;  // 1.0: the game's own value, nothing changed

/// The tenths a word stands for (its place in the table's words, plus one); none if it is no multiplier word.
inline std::optional<int> TenthsOf(std::wstring_view word) noexcept
{
    for (std::size_t i = 0; i < platform::settings::kTenthsWords.size(); ++i)
    {
        if (word == platform::settings::kTenthsWords[i])
        {
            return static_cast<int>(i) + 1;
        }
    }
    return std::nullopt;
}

/// The word for `tenths` (1 to 100), or "1.0" outside them.
inline const wchar_t* WordOf(int tenths) noexcept
{
    return tenths >= 1 && tenths <= static_cast<int>(platform::settings::kTenthsMax)
               ? platform::settings::kTenthsWords[static_cast<std::size_t>(tenths - 1)]
               : L"1.0";
}

/// `tenths / 10` as the float the game's own difficulty step multiplies by.
inline float Multiplier(int tenths) noexcept
{
    return static_cast<float>(tenths) / 10.0f;
}

/// `value` times `tenths / 10`, rounded as the game rounds its rates (float, `(int)(2x + 0.5) >> 1`), at least 1 for a
/// positive value; a value of 0 or less is returned as it is. Capped below INT_MAX / 2 so the doubling cannot overflow.
inline std::int32_t Scaled(std::int32_t value, int tenths) noexcept
{
    if (value <= 0 || tenths == kOne)
    {
        return value;
    }
    const float m     = Multiplier(tenths);
    const float twice = static_cast<float>(value) * m + static_cast<float>(value) * m + 0.5f;
    if (twice >= static_cast<float>(INT_MAX - 1))
    {
        return INT_MAX / 2;
    }
    // Rounded to nearest (even on a tie) as the game's conversion instruction does, then halved: a half rounds up.
    const std::int32_t scaled = static_cast<std::int32_t>(std::lrintf(twice)) >> 1;
    return scaled < 1 ? 1 : scaled;
}

/// The percent the AP bonus row carries for one award: the game's own row (`stock`) while its bonus applies (`open`:
/// the story cleared, or the save's own bonus on), else 100; times `tenths / 10`, rounded half up to a whole percent.
inline std::int32_t AwardPercent(std::int32_t stock, bool open, int tenths) noexcept
{
    const std::int64_t base = open ? stock : 100;
    const std::int64_t pct  = (base * tenths + 5) / 10;
    return pct > INT_MAX ? INT_MAX : static_cast<std::int32_t>(pct);
}

}  // namespace lss::battle::battle_rates_rules
