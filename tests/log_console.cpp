// The mod's log for the host tests of code that logs (platform/log.h's four symbols): every level, to the console.
// Linked by the tests that do not define these themselves.

#include "platform/log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace lss::platform {

namespace {
std::atomic<unsigned> g_lines{0};
}

void WriteLine(std::wstring_view line) noexcept
{
    g_lines.fetch_add(1, std::memory_order_relaxed);
    std::printf("  log: %ls\n", std::wstring(line).c_str());
}

unsigned LinesWritten() noexcept
{
    return g_lines.load(std::memory_order_relaxed);
}

namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Trace)};
}

const wchar_t* LevelName(Level) noexcept
{
    return L"level";
}

}  // namespace lss::platform
