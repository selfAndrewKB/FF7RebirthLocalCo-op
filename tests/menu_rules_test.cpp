// Host-side tests for player_two::menu_rules, run without the game.
//
// Pad 2's press gate and the taken-key set; the world-slow value and the wait sound shared by the two menus; the join
// Setup's pacing and readiness; the follow rule.

#include "player_two/menu_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::player_two::menu_rules;

void TestPressGate()
{
    std::puts("press gate: every condition must hold");
    EXPECT(PressAllowed(1, 1, 1, 1, true));
    EXPECT(!PressAllowed(0, 1, 1, 1, true));
    EXPECT(!PressAllowed(1, 0, 1, 1, true));
    EXPECT(!PressAllowed(1, 1, 0, 1, true));
    EXPECT(!PressAllowed(1, 1, 1, 0, true));
    EXPECT(!PressAllowed(1, 1, 1, 1, false));
    EXPECT(PressAllowed(2, 7, 3, 1, true));
}

void TestTakenKeys()
{
    std::puts("taken keys: noted once; full at 32; a key of 0 is never taken; a key is forgotten once taken");
    TakenKeys taken;
    EXPECT(!taken.Take(0x10));
    EXPECT(taken.NoteTaken(0x10) == TakenKeys::Note::Noted);
    EXPECT(taken.NoteTaken(0x10) == TakenKeys::Note::Already);
    EXPECT(taken.Take(0x10));
    EXPECT(!taken.Take(0x10));  // forgotten
    EXPECT(taken.NoteTaken(0x10) == TakenKeys::Note::Noted);
    EXPECT(!taken.Take(0));
    for (std::uint64_t key = 0x20; key < 0x20 + TakenKeys::kCapacity - 1; ++key)
    {
        EXPECT(taken.NoteTaken(key) == TakenKeys::Note::Noted);
    }
    EXPECT(taken.NoteTaken(0x99) == TakenKeys::Note::Full);
    EXPECT(taken.NoteTaken(0x20) == TakenKeys::Note::Already);  // a held key is still known when full
    EXPECT(taken.Take(0x25));
    EXPECT(taken.NoteTaken(0x99) == TakenKeys::Note::Noted);  // the freed slot
    EXPECT(taken.Take(0x99));
}

void TestSharedState()
{
    std::puts("slow value: the smaller when player 1's came this frame; wait sound: skipped while the other menu waits");
    EXPECT(SlowWritten(true, 0.5f, 1.0f) == 0.5f);
    EXPECT(SlowWritten(true, 1.0f, 0.25f) == 0.25f);
    EXPECT(SlowWritten(false, 0.5f, 1.0f) == 1.0f);

    for (const std::int32_t other : {1, 4, 5})
    {
        EXPECT(SkipWaitSoundOff(1, 0, other));
    }
    EXPECT(!SkipWaitSoundOff(1, 0, 0));
    EXPECT(!SkipWaitSoundOff(1, 0, 2));
    EXPECT(!SkipWaitSoundOff(1, 5, 0));  // player 1's own state does not matter in its pass
    for (const std::int32_t other : {1, 5})
    {
        EXPECT(SkipWaitSoundOff(2, other, 0));
    }
    EXPECT(!SkipWaitSoundOff(2, 4, 0));  // player 1's 4 does not hold player 2's sound
    EXPECT(!SkipWaitSoundOff(2, 0, 5));
    EXPECT(!SkipWaitSoundOff(0, 1, 1));
}

void TestJoinSetup()
{
    std::puts("join setup: the first five tries on consecutive ticks, then every 30th tick, never given up; readiness");
    unsigned waitTicks = 0;
    for (int tries = 0; tries < kSetupTries; ++tries)
    {
        EXPECT(JoinTryDue(tries, waitTicks));
    }
    EXPECT(waitTicks == 0);
    int due = 0;
    for (int tick = 0; tick < 300; ++tick)
    {
        due += JoinTryDue(kSetupTries, waitTicks) ? 1 : 0;
    }
    EXPECT(due == 10);
    EXPECT(waitTicks == 300);
    due = 0;
    for (int tick = 0; tick < 3000; ++tick)
    {
        due += JoinTryDue(1000, waitTicks) ? 1 : 0;
    }
    EXPECT(due == 100);  // never given up

    EXPECT(GameSetUp(0x1000, 1, 0, 0));
    EXPECT(!GameSetUp(0, 1, 0, 0));
    EXPECT(!GameSetUp(0x1000, 0, 0, 0));
    EXPECT(!GameSetUp(0x1000, 1, 1, 0));
    EXPECT(!GameSetUp(0x1000, 1, 0, 1));
}

void TestCanFollow()
{
    std::puts("can follow: built, on the game thread, the game's own controller named by the UI system; a call that "
              "needs the member only while player 2 plays");
    EXPECT(CanFollow(true, false, true, true, false, false, true, false, true));
    EXPECT(!CanFollow(true, false, true, true, true, false, true, false, true));  // needs the member: player 2 out
    EXPECT(CanFollow(true, false, true, true, true, true, true, false, true));
    EXPECT(!CanFollow(false, false, true, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, true, true, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, false, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, true, false, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, true, true, false, true, false, false, true));
    EXPECT(!CanFollow(true, false, true, true, false, true, true, true, true));  // the call was on player 2's own
    EXPECT(!CanFollow(true, false, true, true, false, true, true, false, false));
}

}  // namespace

int main()
{
    TestPressGate();
    TestTakenKeys();
    TestSharedState();
    TestJoinSetup();
    TestCanFollow();

    return lss::test::Report();
}
