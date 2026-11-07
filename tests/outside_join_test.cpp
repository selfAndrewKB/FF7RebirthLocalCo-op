// Host-side tests for session::outside_join, run without the game.
//
// The two waits of a seat's player who is out: the rejoin after a map load (timed, 180 ready ticks, 5 tries) and the
// wait for a free party member (untimed, a 30-tick slow check, a look window after a party change, 5 failed joins). The
// session's member test and join are the caller's; here the schedule alone is pinned.

#include "session/outside_join.h"

#include "expect.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace lss::session::outside_join;
using namespace std::chrono_literals;

using Clock = OutsideJoin::Clock;

const Clock::time_point kT0 = Clock::time_point(1000s);

bool Is(const Step& step, Action action, Reason reason = Reason::None)
{
    return step.action == action && step.reason == reason;
}

/// Ticks `count` times, ready, and returns the last step.
Step Ticks(OutsideJoin& wait, int count, std::uint32_t& looks, Clock::time_point now = kT0, bool ready = true)
{
    Step last;
    for (int i = 0; i < count; ++i)
    {
        last = wait.Tick(now, false, ready, looks);
    }
    return last;
}

void TestRejoinSettlesThenTries()
{
    std::puts("rejoin: a join on the 180th ready tick, five tries, then the wait ends; a not-ready tick restarts the settle");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    EXPECT(wait.Current() == Mode::None);
    EXPECT(Is(wait.Tick(kT0, false, true, looks), Action::None));  // nothing pending

    wait.BeginRejoin(kT0);
    EXPECT(wait.Current() == Mode::Rejoin);
    EXPECT(Is(Ticks(wait, kSettleTicks - 1, looks), Action::None));
    const Step first = wait.Tick(kT0 + 1s, false, true, looks);
    EXPECT(Is(first, Action::TryJoin) && first.attempt == 1);
    EXPECT(Is(wait.AfterJoin(Mode::Rejoin, false), Action::None));
    EXPECT(wait.Current() == Mode::Rejoin);

    EXPECT(Is(Ticks(wait, 100, looks), Action::None));
    EXPECT(Is(wait.Tick(kT0, false, false, looks), Action::None));  // not ready: the settle restarts
    EXPECT(Is(Ticks(wait, kSettleTicks - 1, looks), Action::None));
    const Step second = Ticks(wait, 1, looks);
    EXPECT(Is(second, Action::TryJoin) && second.attempt == 2);
    wait.AfterJoin(Mode::Rejoin, false);

    for (std::uint32_t attempt = 3; attempt <= kTries; ++attempt)
    {
        const Step step = Ticks(wait, kSettleTicks, looks);
        EXPECT(Is(step, Action::TryJoin) && step.attempt == attempt);
        wait.AfterJoin(Mode::Rejoin, false);
    }
    EXPECT(Is(Ticks(wait, kSettleTicks, looks), Action::GiveUp, Reason::RejoinTriesExhausted));
    EXPECT(wait.Current() == Mode::None);
    EXPECT(Is(wait.Tick(kT0, false, true, looks), Action::None));
}

void TestRejoinEndsWhenInOrOnTime()
{
    std::puts("rejoin: ends once player 2 is in; gives up after 120 s, or when joins are refused");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginRejoin(kT0);
    EXPECT(Is(Ticks(wait, kSettleTicks, looks), Action::TryJoin));
    EXPECT(Is(wait.AfterJoin(Mode::Rejoin, true), Action::None));
    EXPECT(wait.Current() == Mode::None);

    wait.BeginRejoin(kT0);
    EXPECT(Is(wait.Tick(kT0 + kRejoinWait, false, true, looks), Action::None));  // exactly the wait: still on
    EXPECT(Is(wait.Tick(kT0 + kRejoinWait + 1ms, false, true, looks), Action::GiveUp, Reason::RejoinTimedOut));
    EXPECT(wait.Current() == Mode::None);

    wait.BeginRejoin(kT0);
    EXPECT(Is(wait.Tick(kT0, true, true, looks), Action::GiveUp, Reason::RejoinRefused));
    EXPECT(wait.Current() == Mode::None);

    wait.BeginRejoin(kT0);
    EXPECT(Is(wait.Tick(kT0 + kRejoinWait + 1s, true, true, looks), Action::GiveUp, Reason::RejoinTimedOut));  // time first
}

void TestAwaitSlowCheck()
{
    std::puts("await: no time limit; after 180 ready ticks the member test runs every 30th tick");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginAwait();
    EXPECT(wait.Current() == Mode::Await);
    EXPECT(Is(Ticks(wait, kSettleTicks, looks, kT0 + 10h), Action::None));  // the settle, hours later: still waiting
    EXPECT(Is(Ticks(wait, kAwaitCheckTicks - 1, looks), Action::None));
    const Step step = Ticks(wait, 1, looks);
    EXPECT(Is(step, Action::TryJoin) && !step.looking);
    EXPECT(Is(Ticks(wait, kAwaitCheckTicks - 1, looks), Action::None));
    EXPECT(Is(Ticks(wait, 1, looks), Action::TryJoin));

    EXPECT(Is(wait.Tick(kT0, false, false, looks), Action::None));  // not ready: the settle restarts
    EXPECT(Is(Ticks(wait, kSettleTicks, looks), Action::None));
    EXPECT(Is(Ticks(wait, kAwaitCheckTicks, looks), Action::TryJoin));
}

void TestAwaitFromTheField()
{
    std::puts("await from a running field: the member test runs on the next ready tick, then every 30th");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginAwait(true);
    EXPECT(wait.Current() == Mode::Await);
    const Step first = Ticks(wait, 1, looks);
    EXPECT(Is(first, Action::TryJoin) && !first.looking);
    EXPECT(Is(Ticks(wait, kAwaitCheckTicks - 1, looks), Action::None));
    EXPECT(Is(Ticks(wait, 1, looks), Action::TryJoin));
    EXPECT(Is(wait.Tick(kT0, false, false, looks), Action::None));  // not ready after all: the settle runs in full
    EXPECT(Is(Ticks(wait, kSettleTicks - 1, looks), Action::None));
    EXPECT(Is(Ticks(wait, 1, looks), Action::None));
    EXPECT(Is(Ticks(wait, kAwaitCheckTicks - 1, looks), Action::None));
    EXPECT(Is(Ticks(wait, 1, looks), Action::TryJoin));
}

void TestAwaitLookWindow()
{
    std::puts("await: a look window tries every tick and counts down only while ready");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginAwait();
    Ticks(wait, kSettleTicks, looks);
    looks = 3;
    EXPECT(Is(wait.Tick(kT0, false, false, looks), Action::None));
    EXPECT(looks == 3);  // not ready: the window keeps its ticks
    Ticks(wait, kSettleTicks, looks);
    EXPECT(looks == 3);  // the settle does not count down either
    for (int i = 3; i > 0; --i)
    {
        const Step step = Ticks(wait, 1, looks);
        EXPECT(Is(step, Action::TryJoin) && step.looking);
        EXPECT(looks == static_cast<std::uint32_t>(i - 1));
    }
    EXPECT(Is(Ticks(wait, 1, looks), Action::None));  // the window is spent: the slow check again
}

void TestAwaitEnds()
{
    std::puts("await: ends once player 2 is in; five joins that did not add player 2 give up; refused joins end it");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginAwait();
    Ticks(wait, kSettleTicks + kAwaitCheckTicks, looks);
    EXPECT(Is(wait.AfterJoin(Mode::Await, true), Action::None));
    EXPECT(wait.Current() == Mode::None);

    wait.BeginAwait();
    for (std::uint32_t failed = 1; failed < kTries; ++failed)
    {
        EXPECT(Is(wait.AfterJoin(Mode::Await, false), Action::None));
        EXPECT(wait.Current() == Mode::Await);
    }
    EXPECT(Is(wait.AfterJoin(Mode::Await, false), Action::GiveUp, Reason::AwaitTriesExhausted));
    EXPECT(wait.Current() == Mode::None);

    wait.BeginAwait();
    EXPECT(Is(wait.Tick(kT0, true, true, looks), Action::GiveUp, Reason::AwaitRefused));
    EXPECT(wait.Current() == Mode::None);
}

void TestJoinThatChangesTheWait()
{
    std::puts("a join that changed the wait counts nothing: a rejoin's join that started an await, a join that ended it");
    OutsideJoin   wait;
    std::uint32_t looks = 0;
    wait.BeginRejoin(kT0);
    EXPECT(Is(Ticks(wait, kSettleTicks, looks), Action::TryJoin));
    wait.BeginAwait();  // the join found no free member
    EXPECT(Is(wait.AfterJoin(Mode::Rejoin, false), Action::None));
    EXPECT(wait.Current() == Mode::Await);
    for (int i = 0; i < 10; ++i)
    {
        // Ten joins that did not add the seat's player, each after the wait was replaced: never a give-up.
        wait.BeginAwait();
        EXPECT(Is(wait.AfterJoin(Mode::Rejoin, false), Action::None));
    }
    EXPECT(wait.Current() == Mode::Await);

    wait.End();  // the failed join asked for a leave
    EXPECT(Is(wait.AfterJoin(Mode::Await, false), Action::None));
    EXPECT(wait.Current() == Mode::None);
}

}  // namespace

int main()
{
    TestRejoinSettlesThenTries();
    TestRejoinEndsWhenInOrOnTime();
    TestAwaitSlowCheck();
    TestAwaitFromTheField();
    TestAwaitLookWindow();
    TestAwaitEnds();
    TestJoinThatChangesTheWait();

    return lss::test::Report();
}
