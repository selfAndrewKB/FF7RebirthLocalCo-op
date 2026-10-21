// Host-side tests for platform/every, run without the game.
//
// The periodic latch: the first ask is due, then one ask per period, counted from the last due ask; Never until the
// first due; Reset makes the next ask due; Restart counts the period from a moment. Take gives a counter's value and
// zeroes it.

#include "platform/every.h"

#include "expect.h"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace {

using lss::platform::Every;
using lss::platform::Take;
using namespace std::chrono_literals;

using Clock = Every::Clock;

void TestDue()
{
    std::puts("every: the first ask is due, then one per period counted from the last due ask");
    const Clock::time_point t0 = Clock::time_point{} + 100s;
    Every                   every{10s};
    EXPECT(every.Never());
    EXPECT(every.Due(t0));
    EXPECT(!every.Never());
    EXPECT(!every.Due(t0 + 5s));
    EXPECT(!every.Due(t0 + 9s));
    EXPECT(every.Due(t0 + 10s));
    EXPECT(!every.Due(t0 + 19s));
    EXPECT(every.Due(t0 + 25s));  // the period counts from the last due ask (t0 + 10 s), not from the first
    EXPECT(!every.Due(t0 + 34s));
    EXPECT(every.Due(t0 + 35s));
}

void TestResetAndRestart()
{
    std::puts("every: Reset makes the next ask due; Restart counts the period from the moment given");
    const Clock::time_point t0 = Clock::time_point{} + 100s;
    Every                   every{10s};
    EXPECT(every.Due(t0));
    EXPECT(!every.Due(t0 + 1s));
    every.Reset();
    EXPECT(every.Never());
    EXPECT(every.Due(t0 + 1s));
    every.Restart(t0 + 30s);
    EXPECT(!every.Never());
    EXPECT(!every.Due(t0 + 35s));
    EXPECT(!every.Due(t0 + 39s));
    EXPECT(every.Due(t0 + 40s));
}

void TestTake()
{
    std::puts("every: Take gives the counter's value and zeroes it");
    std::atomic<unsigned> counter{5};
    EXPECT(Take(counter) == 5);
    EXPECT(counter.load() == 0);
    EXPECT(Take(counter) == 0);
    std::atomic<std::uint64_t> wide{1ull << 40};
    EXPECT(Take(wide) == (1ull << 40));
    EXPECT(wide.load() == 0);
}

}  // namespace

int main()
{
    TestDue();
    TestResetAndRestart();
    TestTake();

    return lss::test::Report();
}
