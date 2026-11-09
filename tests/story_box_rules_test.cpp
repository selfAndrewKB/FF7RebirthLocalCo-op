// Host-side tests for world/story_box_rules.h: which story-box entries of a held member wait, when a waiting one fires,
// the reach test against a placed box, and whether a straight run passes through it. Run without the game. The boxes
// are samples in centimetres: a long box 20 x 4 m, and a narrow one 8 x 2 m with the point a run goes to beyond it.

#include "world/story_box_rules.h"

#include "expect.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace lss::world::story_box_rules;

constexpr Entry kMemberWalksIntoItsOwnBox{.heldByPlayer = true, .levelSentIt = false, .groupSet = true, .groupNamesLeader = false};

void TestAMemberOnlyBoxWaitsForAHeldMember()
{
    std::puts("a member-only box entered by a member its player holds waits");
    EXPECT(HoldBack(kMemberWalksIntoItsOwnBox));
}

void TestEverythingElseIsTheGames()
{
    std::puts("an AI ally, a level move, a party box and a group-less box are the game's");
    Entry onAi = kMemberWalksIntoItsOwnBox;
    onAi.heldByPlayer = false;
    EXPECT(!HoldBack(onAi));

    Entry sent = kMemberWalksIntoItsOwnBox;
    sent.levelSentIt = true;
    EXPECT(!HoldBack(sent));

    Entry party = kMemberWalksIntoItsOwnBox;
    party.groupNamesLeader = true;
    EXPECT(!HoldBack(party));

    Entry groupless = kMemberWalksIntoItsOwnBox;
    groupless.groupSet = false;
    EXPECT(!HoldBack(groupless));
}

constexpr Waiting kStillWaiting{.boxGone = false, .seated = true, .handedToAi = false, .inCinema = false, .levelMoves = false,
                                .playerOneReached = false, .awaitsRun = false, .ranIn = false, .runOver = false};

void TestAWaitingEntryWaits()
{
    std::puts("nothing changed: it waits");
    EXPECT(Next(kStillWaiting) == Step::Wait);
}

void TestEachReleaseReplays()
{
    std::puts("a hand to the AI, a leave, a level move or player 1 at the box replays it");
    Waiting w = kStillWaiting;
    w.handedToAi = true;
    EXPECT(Next(w) == Step::Replay);

    w = kStillWaiting;
    w.seated = false;
    EXPECT(Next(w) == Step::Replay);

    w = kStillWaiting;
    w.levelMoves = true;
    EXPECT(Next(w) == Step::Replay);

    w = kStillWaiting;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Replay);
}

void TestNothingFiresInACutscene()
{
    std::puts("nothing fires during a cutscene");
    Waiting w = kStillWaiting;
    w.handedToAi = true;
    w.inCinema = true;
    EXPECT(Next(w) == Step::Wait);
}

void TestAGoneBoxIsLostNotReplayed()
{
    std::puts("a box gone first is lost, not replayed");
    Waiting w = kStillWaiting;
    w.boxGone = true;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Lost);
}

constexpr Box kLongBox{.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .at = {1810.0f, -36030.0f, 0.0f}, .scale = {1.0f, 1.0f, 1.0f},
                       .extent = {1000.0f, 200.0f, 200.0f}};

void TestInsideAndMarginReach()
{
    std::puts("player 1 reaches the box within 3 m of its sides, not a floor above");
    const float centre[3] = {1810.0f, -36030.0f, 0.0f};
    EXPECT(Reaches(kLongBox, centre));
    const float justOutside[3] = {1810.0f, -36030.0f + 200.0f + 299.0f, 0.0f};
    EXPECT(Reaches(kLongBox, justOutside));
    const float farOutside[3] = {1810.0f, -36030.0f + 200.0f + 301.0f, 0.0f};
    EXPECT(!Reaches(kLongBox, farOutside));
    const float alongTheLongSide[3] = {1810.0f + 1250.0f, -36030.0f, 0.0f};
    EXPECT(Reaches(kLongBox, alongTheLongSide));
    const float aFloorAbove[3] = {1810.0f, -36030.0f, 401.0f};
    EXPECT(!Reaches(kLongBox, aFloorAbove));
}

void TestTheScaleCounts()
{
    std::puts("the component's scale counts");
    Box scaled = kLongBox;
    scaled.extent[1] = 100.0f;
    scaled.scale[1]  = 2.0f;
    const float justOutside[3] = {1810.0f, -36030.0f + 200.0f + 299.0f, 0.0f};
    EXPECT(Reaches(scaled, justOutside));
}

void TestTheRotationCounts()
{
    std::puts("the component's rotation counts");
    Box turned = kLongBox;
    const float half = std::sqrt(0.5f);
    turned.rotation[2] = half;
    turned.rotation[3] = half;
    const float alongWorldY[3] = {1810.0f, -36030.0f + 1250.0f, 0.0f};
    EXPECT(Reaches(turned, alongWorldY));
    const float alongWorldX[3] = {1810.0f + 1250.0f, -36030.0f, 0.0f};
    EXPECT(!Reaches(turned, alongWorldX));
}

void TestANanNeverReaches()
{
    std::puts("a NaN never reaches");
    const float nan[3] = {std::nanf(""), -36030.0f, 0.0f};
    EXPECT(!Reaches(kLongBox, nan));
}

void TestABroughtBackMemberWaitsForItsOwnRun()
{
    std::puts("a member brought back waits for its own run, taken by the game's own enter");
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.levelMoves = true;
    EXPECT(Next(w) == Step::Wait);
    w.ranIn = true;
    EXPECT(Next(w) == Step::Taken);
    w.handedToAi = false;
    EXPECT(Next(w) == Step::Taken);
}

void TestABroughtBackMemberIsNeverLost()
{
    std::puts("a member brought back still fires late, never lost");
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Replay);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = false;
    EXPECT(Next(w) == Step::Replay);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.seated     = false;
    EXPECT(Next(w) == Step::Replay);
    w.boxGone = true;
    EXPECT(Next(w) == Step::Lost);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.inCinema   = true;
    w.ranIn      = true;
    EXPECT(Next(w) == Step::Wait);
}

void TestARunThatMissesItsBoxStillFiresIt()
{
    std::puts("a run that misses its box fires it at its point, stopped, or at the limit");
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    EXPECT(Next(w) == Step::Wait);
    w.runOver = true;
    EXPECT(Next(w) == Step::Replay);
    EXPECT(RunOver(150.0f, 0.0f, 0, 0));
    EXPECT(!RunOver(5000.0f, 0.0f, 8000, 100));
    EXPECT(RunOver(5000.0f, 0.0f, 3000, kRunStillMs));
    EXPECT(!RunOver(5000.0f, 0.0f, 3000, kRunStillMs - 1));
    EXPECT(RunOver(5000.0f, 0.0f, kAwaitRunLimitMs, 0));
}

constexpr Box kNarrowBox{.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .at = {854.0f, -37606.0f, -4890.0f}, .scale = {1.0f, 1.0f, 1.0f},
                         .extent = {400.0f, 100.0f, 40.0f}};
constexpr float kRunPoint[3] = {490.0f, -38330.0f, -4920.0f};

void TestARunFromBehindTheBoxCrossesIt()
{
    std::puts("a run from behind the box passes through it");
    const float farBehind[3] = {3221.0f, -32878.0f, -4885.0f};
    EXPECT(RunCrosses(kNarrowBox, farBehind, kRunPoint));
    const float nearBehind[3] = {1200.0f, -36800.0f, -4880.0f};
    EXPECT(RunCrosses(kNarrowBox, nearBehind, kRunPoint));
}

void TestARunFromPastTheBoxMissesIt()
{
    std::puts("a run from past the box, or beside it beyond the margin, misses it");
    const float pastIt[3] = {300.0f, -42000.0f, -4920.0f};
    EXPECT(!RunCrosses(kNarrowBox, pastIt, kRunPoint));
    const float aside[3]  = {854.0f + 400.0f + 60.0f, -37000.0f, -4890.0f};
    const float aside2[3] = {854.0f + 400.0f + 60.0f, -38000.0f, -4890.0f};
    EXPECT(!RunCrosses(kNarrowBox, aside, aside2));
    const float grazing[3]  = {854.0f + 400.0f + 40.0f, -37000.0f, -4890.0f};
    const float grazing2[3] = {854.0f + 400.0f + 40.0f, -38000.0f, -4890.0f};
    EXPECT(RunCrosses(kNarrowBox, grazing, grazing2));
}

void TestARunStartingOrEndingInsideCrosses()
{
    std::puts("a run starting or ending inside crosses; a NaN or an infinity never does");
    const float inside[3] = {854.0f, -37606.0f, -4890.0f};
    EXPECT(RunCrosses(kNarrowBox, inside, kRunPoint));
    EXPECT(RunCrosses(kNarrowBox, inside, inside));
    const float nan[3] = {std::nanf(""), -37606.0f, -4890.0f};
    EXPECT(!RunCrosses(kNarrowBox, nan, kRunPoint));
    const float inf[3] = {std::numeric_limits<float>::infinity(), -37606.0f, -4890.0f};
    EXPECT(!RunCrosses(kNarrowBox, inf, kRunPoint));
}

}  // namespace

int main()
{
    TestAMemberOnlyBoxWaitsForAHeldMember();
    TestEverythingElseIsTheGames();
    TestAWaitingEntryWaits();
    TestEachReleaseReplays();
    TestNothingFiresInACutscene();
    TestAGoneBoxIsLostNotReplayed();
    TestInsideAndMarginReach();
    TestTheScaleCounts();
    TestTheRotationCounts();
    TestANanNeverReaches();
    TestABroughtBackMemberWaitsForItsOwnRun();
    TestABroughtBackMemberIsNeverLost();
    TestARunThatMissesItsBoxStillFiresIt();
    TestARunFromBehindTheBoxCrossesIt();
    TestARunFromPastTheBoxMissesIt();
    TestARunStartingOrEndingInsideCrosses();
    return lss::test::Report();
}
