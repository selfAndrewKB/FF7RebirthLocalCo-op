#pragma once

// A thing done at most once per period, and the counters it reports.
//
// The modules' periodic Debug lines (a status every 10 s, a summary every minute) ask Every::Due at
// their tick and write the line when it says so; what the line reports is a count taken and zeroed
// with Take. The first ask is due; a Reset makes the next ask due; a Restart starts the period anew
// (a ride's summary a period after it began).

#include <atomic>
#include <chrono>

namespace lss::platform {

class Every
{
public:
    using Clock = std::chrono::steady_clock;

    constexpr explicit Every(Clock::duration period) noexcept : period_(period) {}

    /// True once the period has passed since the last true (the first ask is true); `now` is then the moment
    /// the next period counts from.
    bool Due(Clock::time_point now) noexcept
    {
        if (now - at_ < period_)
        {
            return false;
        }
        at_ = now;
        return true;
    }

    /// No ask was due yet.
    bool Never() const noexcept
    {
        return at_ == Clock::time_point{};
    }

    /// The next ask is due.
    void Reset() noexcept
    {
        at_ = {};
    }

    /// The period starts anew at `now`.
    void Restart(Clock::time_point now) noexcept
    {
        at_ = now;
    }

private:
    Clock::duration   period_;
    Clock::time_point at_{};
};

/// A counter's value, taken and zeroed: what a periodic line reports since the last one.
template <class T>
T Take(std::atomic<T>& counter) noexcept
{
    return counter.exchange(0, std::memory_order_relaxed);
}

}  // namespace lss::platform
