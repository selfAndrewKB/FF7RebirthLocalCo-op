#pragma once

// The log levels' names, as the control command `log-level` and the mod folder's log-level.txt spell them.

#include "platform/log.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace lss::platform {

inline constexpr std::array<std::wstring_view, 5> kLevelWords = {L"error", L"warning", L"info", L"debug", L"trace"};

/// The level a word names, if any.
constexpr std::optional<Level> ParseLevel(std::wstring_view word) noexcept
{
    for (std::size_t i = 0; i < kLevelWords.size(); ++i)
    {
        if (word == kLevelWords[i])
        {
            return static_cast<Level>(i);
        }
    }
    return std::nullopt;
}

/// A level's word ("info" for a value outside the levels).
constexpr std::wstring_view LevelWord(Level level) noexcept
{
    const auto i = static_cast<std::size_t>(level);
    return i < kLevelWords.size() ? kLevelWords[i] : kLevelWords[2];
}

}  // namespace lss::platform
