// Host-side tests for the window span's pure rules (views/window_span_rules.h): the wait for a steady game window
// before the settings file's monitor-mode try, and its give-up.

#include "views/window_span_rules.h"

#include "expect.h"

#include <cstdio>
#include <optional>

namespace {

using lss::views::monitor_row::Rect;
using lss::views::window_span::PresetWait;
using Step = PresetWait::Step;

constexpr Rect kSmall{0, 0, 1280, 720};
constexpr Rect kFull{0, 0, 2560, 1440};

void TestSteadyThenTry()
{
    std::puts("preset wait: the try comes once the rect has been read the same for the run of ticks");
    PresetWait wait;
    // The first read sets the rect; the run counts from the second.
    EXPECT(wait.Next(kSmall, 3, 100) == Step::Wait);
    EXPECT(wait.Next(kSmall, 3, 100) == Step::Wait);
    EXPECT(wait.Next(kSmall, 3, 100) == Step::Wait);
    EXPECT(wait.Next(kSmall, 3, 100) == Step::Try);
    EXPECT(wait.rect == kSmall && wait.ticks == 4);
}

void TestAChangeRestartsTheRun()
{
    std::puts("preset wait: a changed rect (the engine sizing the window) restarts the run");
    PresetWait wait;
    wait.Next(kSmall, 3, 100);
    wait.Next(kSmall, 3, 100);
    EXPECT(wait.Next(kFull, 3, 100) == Step::Wait);
    EXPECT(wait.steady == 0 && wait.rect == kFull);
    wait.Next(kFull, 3, 100);
    wait.Next(kFull, 3, 100);
    EXPECT(wait.Next(kFull, 3, 100) == Step::Try);
}

void TestNoWindowRestartsTheRunAndGivesUp()
{
    std::puts("preset wait: a window that cannot be reached counts as not steady; too long a wait gives up");
    PresetWait wait;
    wait.Next(kSmall, 3, 6);
    wait.Next(kSmall, 3, 6);
    EXPECT(wait.Next(std::nullopt, 3, 6) == Step::Wait);
    EXPECT(wait.steady == 0);
    EXPECT(wait.Next(std::nullopt, 3, 6) == Step::Wait);
    EXPECT(wait.Next(std::nullopt, 3, 6) == Step::Wait);
    EXPECT(wait.Next(std::nullopt, 3, 6) == Step::Wait);   // the sixth tick: still within the allowance
    EXPECT(wait.Next(std::nullopt, 3, 6) == Step::GiveUp);  // the seventh: past it
    EXPECT(wait.ticks == 7);

    std::puts("preset wait: a window that keeps moving gives up the same way");
    PresetWait moving;
    Step       last = Step::Wait;
    for (std::int32_t i = 0; i < 10 && last == Step::Wait; ++i)
    {
        last = moving.Next(Rect{0, 0, 100 + i, 100}, 3, 6);
    }
    EXPECT(last == Step::GiveUp && moving.ticks == 7);
}

}  // namespace

int main()
{
    TestSteadyThenTry();
    TestAChangeRestartsTheRun();
    TestNoWindowRestartsTheRunAndGivesUp();
    return lss::test::Report();
}
