// Host-side tests for the platform's small pure rules, run without the game: the log level words, the hitch step, the
// memory alarm, and RIP-relative addressing.

#include "platform/hitch_step.h"
#include "platform/level_names.h"
#include "platform/memory_alarm.h"
#include "platform/rip_relative.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::platform;

void TestLevelNames()
{
    std::puts("levels: the five words round-trip; unknown words are refused; a value outside the levels reads as info");
    for (const auto level : {Level::Error, Level::Warning, Level::Info, Level::Debug, Level::Trace})
    {
        EXPECT(ParseLevel(LevelWord(level)) == level);
    }
    EXPECT(ParseLevel(L"info") == Level::Info);
    EXPECT(ParseLevel(L"trace") == Level::Trace);
    EXPECT(!ParseLevel(L"Info").has_value());
    EXPECT(!ParseLevel(L"").has_value());
    EXPECT(!ParseLevel(L"verbose").has_value());
    EXPECT(LevelWord(static_cast<Level>(9)) == L"info");
}

void TestHitchStep()
{
    std::puts("hitch: the first frame is the average; a hitch leaves it; 31 in a row are the new normal; else the EMA");
    HitchState state;
    EXPECT(HitchStep(state, 16.0, 1.6, 5.0, 30, 0.05) == FrameKind::First);
    EXPECT(state.averageMs == 16.0 && state.run == 0);
    EXPECT(HitchStep(state, 40.0, 1.6, 5.0, 30, 0.05) == FrameKind::Hitch);  // 40 > 25.6 and > 21
    EXPECT(state.averageMs == 16.0 && state.run == 1);
    EXPECT(HitchStep(state, 20.0, 1.6, 5.0, 30, 0.05) == FrameKind::Steady);  // 20 < 25.6
    EXPECT(state.run == 0 && state.averageMs > 16.0 && state.averageMs < 16.3);
    HitchState small{1.0, 0};
    EXPECT(HitchStep(small, 2.0, 1.6, 5.0, 30, 0.05) == FrameKind::Steady);  // over the ratio but not the margin
    for (unsigned i = 0; i < 30; ++i)
    {
        EXPECT(HitchStep(state, 100.0, 1.6, 5.0, 30, 0.05) == FrameKind::Hitch);
    }
    EXPECT(state.run == 30 && state.averageMs < 17.0);
    EXPECT(HitchStep(state, 100.0, 1.6, 5.0, 30, 0.05) == FrameKind::NewNormal);
    EXPECT(state.run == 0 && state.averageMs == 100.0);
}

void TestMemoryAlarm()
{
    std::puts("memory: a jump, a budget-state change, a lasting low state every 10 s, else the period");
    EXPECT(PoolStepped(11, 500, 2));  // 2.2 % of the capacity
    EXPECT(PoolStepped(-11, 500, 2));
    EXPECT(!PoolStepped(9, 500, 2));
    EXPECT(!PoolStepped(9, 0, 2));

    EXPECT(MemoryJump(0.5, 0.0, false, 0.5));
    EXPECT(MemoryJump(-0.5, 0.0, false, 0.5));
    EXPECT(MemoryJump(0.0, 0.6, false, 0.5));
    EXPECT(MemoryJump(0.0, 0.0, true, 0.5));
    EXPECT(!MemoryJump(0.4, -0.4, false, 0.5));

    EXPECT(MemoryAlarm(true, false, false, false, false, 0, 10000));
    EXPECT(MemoryAlarm(false, true, false, false, false, 0, 10000));
    EXPECT(MemoryAlarm(false, false, true, false, false, 0, 10000));
    EXPECT(!MemoryAlarm(false, true, true, false, false, 0, 10000));
    EXPECT(!MemoryAlarm(false, false, false, true, false, 9999, 10000));
    EXPECT(MemoryAlarm(false, false, false, true, false, 10000, 10000));
    EXPECT(MemoryAlarm(false, false, false, false, true, 10000, 10000));

    EXPECT(MemoryLineDue(true, false, 0, 1000, 60000));
    EXPECT(!MemoryLineDue(false, true, 999, 1000, 60000));  // an alarm at most once a second
    EXPECT(MemoryLineDue(false, true, 1000, 1000, 60000));
    EXPECT(!MemoryLineDue(false, false, 59999, 1000, 60000));
    EXPECT(MemoryLineDue(false, false, 60000, 1000, 60000));
}

void TestRipRelative()
{
    std::puts("rip: the target is the instruction's end plus the displacement; a low byte that would carry is refused");
    EXPECT(RipTarget(0x1000, 7, 0x10) == 0x1017);
    EXPECT(RipTarget(0x1000, 7, -0x10) == 0x0FF7);
    EXPECT(RipTarget(0x140000000, 6, -0x100) == 0x13FFFFF06);
    EXPECT(LowByteFits(0x12345600, 8));
    EXPECT(LowByteFits(0x123456F7, 8));
    EXPECT(!LowByteFits(0x123456F8, 8));
    EXPECT(!LowByteFits(-1, 8));  // low byte 0xFF
    EXPECT(LowByteFits(-0x100, 8));
}

}  // namespace

int main()
{
    TestLevelNames();
    TestHitchStep();
    TestMemoryAlarm();
    TestRipRelative();

    return lss::test::Report();
}
