// Host-side tests for session/lead_keep_rules.h: whether another player keeps a member the level walks to its marks,
// and when it goes to its AI at a mark. Run without the game.

#include "session/lead_keep_rules.h"

#include "expect.h"

#include <cstdio>
#include <initializer_list>

namespace {

using namespace lss::session::lead_keep_rules;

void TestNeededMovesAreKeptThenHandedAtTheirPoint()
{
    std::puts("a needed move with a point: kept, then handed at the point");
    EXPECT(Decide(true, true, true, true, false) == Verdict::Keep);
    EXPECT(Decide(true, true, true, true, true) == Verdict::AtMark);
}

void TestAMoveThatSignalsNothingIsKeptThroughout()
{
    std::puts("a needed move with no arrival trigger: kept throughout, never handed");
    EXPECT(Decide(true, true, true, false, false) == Verdict::Keep);
    EXPECT(Decide(true, true, true, false, true) == Verdict::Keep);
}

void TestAMoveWithNoPointIsAGuide()
{
    std::puts("a needed move with no point to mark: a guide");
    EXPECT(Decide(true, true, false, true, false) == Verdict::Guide);
    EXPECT(Decide(true, true, false, true, true) == Verdict::Guide);
}

void TestTheGuideSwitchBringsBackTheGuide()
{
    std::puts("lead-holder guide: every needed move to its AI");
    EXPECT(Decide(true, false, true, true, false) == Verdict::Guide);
    EXPECT(Decide(true, false, true, false, false) == Verdict::Guide);
}

void TestStagingStaysThePlayers()
{
    std::puts("a move nothing needs: the player's, no marker, no hand");
    for (const bool keeps : {false, true})
    {
        for (const bool reached : {false, true})
        {
            EXPECT(Decide(false, keeps, true, true, reached) == Verdict::Staging);
        }
    }
}

void TestOnlyTheGuideAndTheMarkHand()
{
    std::puts("only a guide and the mark hand the member to its AI");
    EXPECT(ToAi(Verdict::Guide));
    EXPECT(ToAi(Verdict::AtMark));
    EXPECT(!ToAi(Verdict::Keep));
    EXPECT(!ToAi(Verdict::Staging));
}

void TestReachedIsTwoDimensionalAndInclusive()
{
    std::puts("the hand radius: 2 m in the plane, its edge included");
    EXPECT(Reached(0.0f, 0.0f));
    EXPECT(Reached(200.0f, 0.0f));
    EXPECT(Reached(120.0f, 160.0f));
    EXPECT(!Reached(120.0f, 161.0f));
    EXPECT(!Reached(0.0f, -201.0f));
}

void TestAWalkIntoTheMembersOwnBoxIsAGuide()
{
    std::puts("a walk whose step moves on at the member's own story box: a guide whatever else holds");
    EXPECT(Decide(false, false, false, false, false, true) == Verdict::Guide);
    EXPECT(Decide(true, true, true, true, false, true) == Verdict::Guide);
    EXPECT(Decide(true, true, true, true, true, true) == Verdict::Guide);
    EXPECT(Decide(false, true, true, true, false, false) == Verdict::Staging);
}

}  // namespace

int main()
{
    TestAWalkIntoTheMembersOwnBoxIsAGuide();
    TestNeededMovesAreKeptThenHandedAtTheirPoint();
    TestAMoveThatSignalsNothingIsKeptThroughout();
    TestAMoveWithNoPointIsAGuide();
    TestTheGuideSwitchBringsBackTheGuide();
    TestStagingStaysThePlayers();
    TestOnlyTheGuideAndTheMarkHand();
    TestReachedIsTwoDimensionalAndInclusive();
    return lss::test::Report();
}
