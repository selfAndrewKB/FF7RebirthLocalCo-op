// Host-side test for platform/frame_watch's clock, run without the game.
//
// Now() is the performance counter in seconds: positive, never decreasing, the same clock on every thread, and it
// advances with wall time (a 20 ms sleep reads as at least 15 ms and well under a second).

#include "platform/frame_watch.h"

#include "expect.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using lss::platform::frame_watch::Now;

void TestMonotone()
{
    std::puts("frame_watch: Now is positive and never decreases");
    const double first = Now();
    EXPECT(first > 0.0);
    double last = first;
    for (int i = 0; i < 1000; ++i)
    {
        const double now = Now();
        EXPECT(now >= last);
        last = now;
    }
}

void TestAdvances()
{
    std::puts("frame_watch: Now advances with wall time");
    const double before = Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double elapsed = Now() - before;
    EXPECT(elapsed >= 0.015);
    EXPECT(elapsed < 1.0);
}

void TestSameClockOnEveryThread()
{
    std::puts("frame_watch: another thread reads the same clock");
    const double before = Now();
    double       other  = 0.0;
    std::thread  reader([&other] { other = Now(); });
    reader.join();
    const double after = Now();
    EXPECT(other >= before);
    EXPECT(other <= after);
}

}  // namespace

int main()
{
    TestMonotone();
    TestAdvances();
    TestSameClockOnEveryThread();

    return lss::test::Report();
}