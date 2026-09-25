#pragma once

// Short descriptions of engine objects for log lines.

#include <string>

namespace lss::player_two {

/// The object's name, "(none)" for null, or "(unnamed)" if it cannot be read.
std::wstring NameOf(const void* object) noexcept;

inline const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

}  // namespace lss::player_two
