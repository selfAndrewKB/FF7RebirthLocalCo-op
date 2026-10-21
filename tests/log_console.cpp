// The mod's log for the host tests of code that logs (platform/log.h's three symbols): every level, to the console.
// Linked by the tests that do not define these themselves.

#include "platform/log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace lss::platform {

void WriteLine(std::wstring_view line) noexcept
{
    std::printf("  log: %ls\n", std::wstring(line).c_str());
}

namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Trace)};
}

const wchar_t* LevelName(Level) noexcept
{
    return L"level";
}

}  // namespace lss::platform
