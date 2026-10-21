#pragma once

// When the memory watch writes a line (platform/memory_watch.h), as pure rules tested on the host
// (tests/memory_alarm_test.cpp): a jump in commit, the card's use or a pool; a change of the card's budget state; a
// lasting low-commit or full-pool state repeated every so often; else the period.

#include <cstdint>

namespace lss::platform {

/// A 32 KB pool change this large (of its capacity, in percent) is logged at once.
constexpr bool PoolStepped(std::int64_t change, std::uint64_t capacity, std::int64_t stepPercent) noexcept
{
    return capacity != 0 && (change >= 0 ? change : -change) * 100 >= static_cast<std::int64_t>(capacity) * stepPercent;
}

/// The process commit or the card's use moved by `jumpGb`, or a pool stepped.
constexpr bool MemoryJump(double commitChangeGb, double cardChangeGb, bool poolStep, double jumpGb) noexcept
{
    return commitChangeGb >= jumpGb || commitChangeGb <= -jumpGb || cardChangeGb >= jumpGb || cardChangeGb <= -jumpGb ||
           poolStep;
}

/// A lasting state (over budget, low commit, a nearly full pool) is not repeated every second: a change of budget state
/// is, and the others every `lowRepeatMs`.
constexpr bool MemoryAlarm(bool jump, bool overBudget, bool loggedOverBudget, bool lowCommit, bool poolHigh,
                           std::uint64_t sinceLastLineMs, std::uint64_t lowRepeatMs) noexcept
{
    return jump || overBudget != loggedOverBudget || ((lowCommit || poolHigh) && sinceLastLineMs >= lowRepeatMs);
}

/// A line is written at the first sample, for an alarm at most once per `minLineMs`, and every `periodMs` otherwise.
constexpr bool MemoryLineDue(bool first, bool alarm, std::uint64_t sinceLastLineMs, std::uint64_t minLineMs,
                             std::uint64_t periodMs) noexcept
{
    return first || (alarm && sinceLastLineMs >= minLineMs) || sinceLastLineMs >= periodMs;
}

}  // namespace lss::platform
