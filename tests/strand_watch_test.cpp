// Host-side tests for StrandWatch, run without the game.

#include "player_two/world/strand_watch.h"

#include "expect.h"

#include <cstdio>
#include <vector>

namespace {

using lss::player_two::StrandEvent;
using lss::player_two::StrandWatch;

constexpr float kTick = 1.0f / 60.0f;

/// Runs `ticks` ticks with the same input and returns the events that were not None.
std::vector<StrandEvent> Run(StrandWatch& watch, bool refused, int ticks)
{
    std::vector<StrandEvent> events;
    for (int i = 0; i < ticks; ++i)
    {
        if (const auto event = watch.Step(refused, kTick); event != StrandEvent::None)
        {
            events.push_back(event);
        }
    }
    return events;
}

void TestIdleNeverStrands()
{
    std::puts("strand: a member standing still (no refused move) never strands, however long");
    StrandWatch watch;
    EXPECT(Run(watch, false, 600).empty());
    EXPECT(!watch.Stranded());
}

void TestHeldPushAsksOnce()
{
    std::puts("strand: refused moves every tick begin a strand and ask for one warp at the hold");
    StrandWatch watch;
    EXPECT(watch.Step(true, kTick) == StrandEvent::Began);
    const auto early = Run(watch, true, 58);  // 58 more ticks: held 0.97 s
    EXPECT(early.empty());
    const auto events = Run(watch, true, 120);
    EXPECT(events.size() == 1 && events[0] == StrandEvent::Warp);
    EXPECT(watch.Stranded() && watch.Held() >= 1.0f);
}

void TestLettingGoEndsWithoutWarp()
{
    std::puts("strand: letting go of the stick before the hold ends the strand after the grace, no warp");
    StrandWatch watch;
    Run(watch, true, 30);
    const auto events = Run(watch, false, 36);  // 0.6 s without a refused move
    EXPECT(events.size() == 1 && events[0] == StrandEvent::Ended);
    EXPECT(!watch.Stranded());
}

void TestShortGapsAreOnePush()
{
    std::puts("strand: gaps within the grace (moves not ticked every frame) still count as one push");
    StrandWatch watch;
    std::vector<StrandEvent> events;
    for (int i = 0; i < 90; ++i)
    {
        if (const auto event = watch.Step(i % 3 == 0, kTick); event != StrandEvent::None)
        {
            events.push_back(event);
        }
    }
    EXPECT(events.size() == 2 && events[0] == StrandEvent::Began && events[1] == StrandEvent::Warp);
}

void TestFallBurstsAreOneStrand()
{
    std::puts("strand: refused falls in bursts ~0.3 s apart (a fall edge, no stick) are one strand that warps");
    StrandWatch watch{lss::player_two::StrandRules{.holdSeconds = 0.3f}};
    std::vector<StrandEvent> events;
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        for (int i = 0; i < 21; ++i)  // 4 refused ticks, then 17 quiet ones (0.28 s), as in the live log
        {
            if (const auto event = watch.Step(i < 4, kTick); event != StrandEvent::None)
            {
                events.push_back(event);
            }
        }
    }
    EXPECT(events.size() == 2 && events[0] == StrandEvent::Began && events[1] == StrandEvent::Warp);
    EXPECT(watch.Stranded());
}

void TestLongGapRestartsTheHold()
{
    std::puts("strand: a gap longer than the grace ends the strand, and the next push holds from zero");
    StrandWatch watch;
    Run(watch, true, 54);   // 0.9 s
    Run(watch, false, 36);  // 0.6 s: ended
    const auto events = Run(watch, true, 36);  // 0.6 s: begins again, too short to ask
    EXPECT(events.size() == 1 && events[0] == StrandEvent::Began);
}

void TestCooldownDelaysTheNextWarp()
{
    std::puts("strand: a new strand right after a warp waits out the cooldown before asking again");
    StrandWatch watch;
    Run(watch, true, 70);                      // began, warp at 1.0 s
    Run(watch, false, 36);                     // ended
    const auto soon = Run(watch, true, 66);    // held 1.1 s, but the cooldown (2 s) has not run out
    EXPECT(soon.size() == 1 && soon[0] == StrandEvent::Began);
    const auto later = Run(watch, true, 60);   // the cooldown runs out while the push is held
    EXPECT(later.size() == 1 && later[0] == StrandEvent::Warp);
}

void TestResetForgetsEverything()
{
    std::puts("strand: Reset forgets the strand and the cooldown");
    StrandWatch watch;
    Run(watch, true, 70);
    watch.Reset();
    EXPECT(!watch.Stranded() && watch.Held() == 0.0f);
    const auto events = Run(watch, true, 70);
    EXPECT(events.size() == 2 && events[1] == StrandEvent::Warp);
}

void TestParkChecksCountOnlyWhileParked()
{
    std::puts("strand: failed safe checks count only while the game holds the character parked");
    EXPECT(lss::player_two::ParkChecks(3, true) == 3);
    EXPECT(lss::player_two::ParkChecks(3, false) == 0);
    EXPECT(lss::player_two::ParkChecks(0, true) == 0);
}

}  // namespace

int main()
{
    TestIdleNeverStrands();
    TestHeldPushAsksOnce();
    TestLettingGoEndsWithoutWarp();
    TestShortGapsAreOnePush();
    TestFallBurstsAreOneStrand();
    TestLongGapRestartsTheHold();
    TestCooldownDelaysTheNextWarp();
    TestResetForgetsEverything();
    TestParkChecksCountOnlyWhileParked();

    return lss::test::Report();
}
