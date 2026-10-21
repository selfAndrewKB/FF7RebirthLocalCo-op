#pragma once

// Short descriptions of engine objects for log lines.

#include <string>

namespace lss::player_two {

/// The object's name, "(none)" for null, or "(unnamed)" if it cannot be read.
std::wstring NameOf(const void* object) noexcept;

/// Who an object is to the players, for a log line: "none", player 1's or player 2's controller, player 2's
/// member, player 1's character, else its name. Any thread (the generation's atomics).
std::wstring Who(const void* object) noexcept;

inline const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

}  // namespace lss::player_two
