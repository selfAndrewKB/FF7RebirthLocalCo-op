// Host-side tests for platform::SettledLog, run without the game.
//
// A steady part logs at once; a moving part logs once it has held still for the set number of offers
// and differs from what was logged; a flicker that returns to the logged value logs nothing.

#include "platform/settled_log.h"

#include "expect.h"

#include <cstdio>

namespace {

using lss::platform::SettledLog;

void TestFirstAndSteady()
{
    std::puts("settled log: the first offer and each steady change log at once");
    SettledLog<int, int> log(3);
    EXPECT(log.Offer(1, 10));
    EXPECT(!log.Offer(1, 10));
    EXPECT(log.Offer(2, 10));
    EXPECT(log.Offer(3, 11));  // a steady change logs even while the moving part moves
    log.Reset();
    EXPECT(log.Offer(3, 11));
}

void TestMovingSettles()
{
    std::puts("settled log: a moving part logs once, where it holds still");
    SettledLog<int, int> log(3);
    EXPECT(log.Offer(1, 10));
    EXPECT(!log.Offer(1, 11));
    EXPECT(!log.Offer(1, 12));
    EXPECT(!log.Offer(1, 13));  // still moving
    EXPECT(!log.Offer(1, 13));  // held 1
    EXPECT(!log.Offer(1, 13));  // held 2
    EXPECT(log.Offer(1, 13));   // held 3: logged
    EXPECT(!log.Offer(1, 13));
    EXPECT(!log.Offer(1, 13));
}

void TestFlickerIgnored()
{
    std::puts("settled log: a flicker back to the logged value logs nothing");
    SettledLog<int, int> log(2);
    EXPECT(log.Offer(1, 0));
    EXPECT(!log.Offer(1, 1));  // a one-frame press
    EXPECT(!log.Offer(1, 0));
    EXPECT(!log.Offer(1, 0));
    EXPECT(!log.Offer(1, 0));
    EXPECT(!log.Offer(1, 0));
}

void TestSteadyResetsMoving()
{
    std::puts("settled log: a steady change takes the moving part as logged");
    SettledLog<int, int> log(2);
    EXPECT(log.Offer(1, 5));
    EXPECT(!log.Offer(1, 6));
    EXPECT(log.Offer(2, 6));   // logged with 6
    EXPECT(!log.Offer(2, 6));
    EXPECT(!log.Offer(2, 6));  // 6 was logged: no second line
}

void TestZeroCountsAsOne()
{
    std::puts("settled log: a hold of 0 offers counts as 1");
    SettledLog<int, int> log(0);
    EXPECT(log.Offer(1, 0));
    EXPECT(!log.Offer(1, 1));
    EXPECT(log.Offer(1, 1));
}

}  // namespace

int main()
{
    TestFirstAndSteady();
    TestMovingSettles();
    TestFlickerIgnored();
    TestSteadyResetsMoving();
    TestZeroCountsAsOne();
    return lss::test::Report();
}
