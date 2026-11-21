// Host-side tests for battle::menu_rules, run without the game.
//
// A seat's pad's press gate and the taken-key set; the world-slow value and the wait sound shared by the two menus;
// the join Setup's pacing and readiness; the follow rule.

#include "battle/menu_rules.h"

#include "expect.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

using namespace lss::battle::menu_rules;

void TestPressGate()
{
    std::puts("press gate: every condition must hold");
    EXPECT(PressAllowed(true, 1, 1, true));
    EXPECT(!PressAllowed(false, 1, 1, true));
    EXPECT(!PressAllowed(true, 0, 1, true));
    EXPECT(!PressAllowed(true, 1, 0, true));
    EXPECT(!PressAllowed(true, 1, 1, false));
    EXPECT(PressAllowed(true, 7, 3, true));
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

/// Two players through the rules over every menu: player 1's menu, then player 2's.
constexpr float SlowOfTwo(bool playerOnesThisFrame, float playerOnes, float mine) noexcept
{
    const SlowValue earlier[1] = {{playerOnesThisFrame, playerOnes}};
    return SlowWrittenAmong(earlier, 1, mine);
}
/// By pass: 1 player 1's tick, 2 player 2's.
constexpr bool SkipOfTwo(int pass, std::int32_t waitOne, std::int32_t waitTwo) noexcept
{
    const std::int32_t waits[2] = {waitOne, waitTwo};
    return SkipWaitSoundOffAmong(pass == 1 ? 0 : pass == 2 ? 1 : -1, waits, 2);
}

void TestSharedState()
{
    std::puts("slow value: the smaller when player 1's came this frame; wait sound: skipped while the other menu waits");
    EXPECT(SlowOfTwo(true, 0.5f, 1.0f) == 0.5f);
    EXPECT(SlowOfTwo(true, 1.0f, 0.25f) == 0.25f);
    EXPECT(SlowOfTwo(false, 0.5f, 1.0f) == 1.0f);

    for (const std::int32_t other : {1, 4, 5})
    {
        EXPECT(SkipOfTwo(1, 0, other));
    }
    EXPECT(!SkipOfTwo(1, 0, 0));
    EXPECT(!SkipOfTwo(1, 0, 2));
    EXPECT(!SkipOfTwo(1, 5, 0));  // player 1's own state does not matter in its pass
    for (const std::int32_t other : {1, 5})
    {
        EXPECT(SkipOfTwo(2, other, 0));
    }
    EXPECT(!SkipOfTwo(2, 4, 0));  // player 1's 4 does not hold player 2's sound
    EXPECT(!SkipOfTwo(2, 0, 5));
    EXPECT(!SkipOfTwo(0, 1, 1));
}

/// The two-player rules as they stood before the seats, verbatim: the references for the rules over every menu.
constexpr float OldSlowWritten(bool playerOnesThisFrame, float playerOnes, float playerTwos) noexcept
{
    return playerOnesThisFrame ? std::min(playerOnes, playerTwos) : playerTwos;
}
constexpr bool OldSkipWaitSoundOff(int pass, std::int32_t waitOne, std::int32_t waitTwo) noexcept
{
    return (pass == 1 && (waitTwo == 1 || waitTwo == 5 || waitTwo == 4)) || (pass == 2 && (waitOne == 1 || waitOne == 5));
}

void TestMenusAsBefore()
{
    std::puts("menus: with two players the rules over every menu give the two-player answers for every input");
    constexpr float kValues[] = {0.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
    for (const bool thisFrame : {false, true})
    {
        for (const float one : kValues)
        {
            for (const float two : kValues)
            {
                EXPECT(SlowOfTwo(thisFrame, one, two) == OldSlowWritten(thisFrame, one, two));
            }
        }
    }
    for (int pass = -1; pass <= 4; ++pass)
    {
        for (std::int32_t one = -1; one <= 7; ++one)
        {
            for (std::int32_t two = -1; two <= 7; ++two)
            {
                EXPECT(SkipOfTwo(pass, one, two) == OldSkipWaitSoundOff(pass, one, two));
            }
        }
    }
}

void TestMenusOfThree()
{
    std::puts("menus: three players, the smallest slow of this frame's earlier menus; the wait sound waits for every "
              "other menu (closing counts only for a later one)");
    const SlowValue earlier[2] = {{true, 0.5f}, {true, 0.25f}};
    EXPECT(SlowWrittenAmong(earlier, 2, 1.0f) == 0.25f);
    const SlowValue stale[2] = {{true, 0.5f}, {false, 0.25f}};
    EXPECT(SlowWrittenAmong(stale, 2, 1.0f) == 0.5f);
    EXPECT(SlowWrittenAmong(stale, 0, 1.0f) == 1.0f);

    const std::int32_t thirdWaits[3] = {0, 0, 1};
    EXPECT(SkipWaitSoundOffAmong(0, thirdWaits, 3));  // player 3 still waits: player 1's sound off waits
    EXPECT(SkipWaitSoundOffAmong(1, thirdWaits, 3));
    EXPECT(!SkipWaitSoundOffAmong(2, thirdWaits, 3));  // its own state does not matter
    const std::int32_t closing[3] = {4, 0, 4};
    EXPECT(SkipWaitSoundOffAmong(1, closing, 3));   // player 3 closes later this frame: it plays the sound
    EXPECT(!SkipWaitSoundOffAmong(2, closing, 3));  // the last to close plays it (player 1's 4 came earlier)
    EXPECT(!SkipWaitSoundOffAmong(-1, thirdWaits, 3));
}

void TestSlowDownSwitch()
{
    std::puts("slow-down switch: off asks full speed; at a switch, the grey and the muffle follow the waiting menus");
    EXPECT(SlowAsked(true, 0.05f) == 0.05f);
    EXPECT(SlowAsked(false, 0.05f) == kFullSpeed);
    EXPECT(SlowAsked(false, 1.0f) == kFullSpeed);
    // Three menus all asking the slow with the switch off: the frame stays at full speed.
    const SlowValue offEarlier[2] = {{true, SlowAsked(false, 0.05f)}, {true, SlowAsked(false, 0.05f)}};
    EXPECT(SlowWrittenAmong(offEarlier, 2, SlowAsked(false, 0.05f)) == kFullSpeed);

    EXPECT(GreyAtSwitch(false, 5, true) == Flip::Lift);
    EXPECT(GreyAtSwitch(false, 4, true) == Flip::Lift);  // closing but still held: taken down
    EXPECT(GreyAtSwitch(false, 5, false) == Flip::Nothing);
    EXPECT(GreyAtSwitch(true, 5, false) == Flip::Raise);
    EXPECT(GreyAtSwitch(true, 1, false) == Flip::Raise);
    EXPECT(GreyAtSwitch(true, 4, false) == Flip::Nothing);  // closing: its close would take it down at once
    EXPECT(GreyAtSwitch(true, 0, false) == Flip::Nothing);
    EXPECT(GreyAtSwitch(true, 5, true) == Flip::Nothing);

    const std::int32_t none[3]    = {0, 0, 0};
    const std::int32_t open[3]    = {0, 5, 0};
    const std::int32_t closing[3] = {4, 0, 0};
    EXPECT(SoundAtSwitch(false, open, 3) == Flip::Lift);
    EXPECT(SoundAtSwitch(false, closing, 3) == Flip::Lift);  // its own close is skipped from now on
    EXPECT(SoundAtSwitch(false, none, 3) == Flip::Nothing);
    EXPECT(SoundAtSwitch(true, open, 3) == Flip::Raise);
    EXPECT(SoundAtSwitch(true, closing, 3) == Flip::Nothing);
    EXPECT(SoundAtSwitch(true, none, 3) == Flip::Nothing);
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
    EXPECT(!CanFollow(true, false, true, true, true, false, true, false, true));  // needs the member: the player is out
    EXPECT(CanFollow(true, false, true, true, true, true, true, false, true));
    EXPECT(!CanFollow(false, false, true, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, true, true, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, false, true, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, true, false, false, true, true, false, true));
    EXPECT(!CanFollow(true, false, true, true, false, true, false, false, true));
    EXPECT(!CanFollow(true, false, true, true, false, true, true, true, true));  // the call was on a seat's own
    EXPECT(!CanFollow(true, false, true, true, false, true, true, false, false));
}

}  // namespace

int main()
{
    TestPressGate();
    TestTakenKeys();
    TestSharedState();
    TestMenusAsBefore();
    TestMenusOfThree();
    TestSlowDownSwitch();
    TestJoinSetup();
    TestCanFollow();

    return lss::test::Report();
}
