// Host-side tests for the held member's script rule (session/field_hold.h), run without the game.

#include "session/field_hold.h"

#include "expect.h"

#include <array>
#include <cstdio>
#include <string_view>

namespace {

using namespace lss::session::field_hold;
using namespace lss::facts;

void TestHoldOf()
{
    std::puts("hold: the base brain and stands are the player's; level moves are judged; other moves and animations go");
    EXPECT(Of(kBrainBase, kAiStateFollow) == Hold::Free);
    EXPECT(Of(kBrainBase, kAiStateLead) == Hold::Free);  // the base brain never holds a script's move
    EXPECT(Of(kBrainLevel, kAiStateStay) == Hold::Free);
    EXPECT(Of(kBrainLevel, kAiStateDoNothing) == Hold::Free);
    EXPECT(Of(kBrainLevel, kAiStateMoveTo) == Hold::Arrival);
    EXPECT(Of(kBrainLevel, kAiStateLead) == Hold::Arrival);
    EXPECT(Of(kBrainLevelGroup, kAiStateLead) == Hold::Arrival);
    EXPECT(Of(16, kAiStateMoveTo) == Hold::Script);  // an event command's walk to a mark
    EXPECT(Of(17, kAiStateDoNothing) == Hold::Free);  // a scene's park
    EXPECT(Of(12, kAiStateDoNothing) == Hold::Free);
    EXPECT(Of(kBrainLevel, kAiStateAnimation) == Hold::Animation);
    EXPECT(Of(10, kAiStateSingleAnimation) == Hold::Animation);
    EXPECT(Of(5, kAiStateFieldAction) == Hold::Free);
    EXPECT(Of(kBrainLevel, kAiStateInterest) == Hold::Free);
    EXPECT(Of(kBrainLevelGroup, kAiStateMoveTo) == Hold::Arrival);
    EXPECT(Of(kBrainBase, kAiStateAnimation) == Hold::Free);  // the base brain is the follow's: never a script's
}

void TestMoveSettingAndPathEnd()
{
    std::puts("settings: the moving Status words without case, a group by its first state; the path's end");
    EXPECT(MoveSetting(L"Lead", 0));
    EXPECT(MoveSetting(L"movetotarget", 0));
    EXPECT(MoveSetting(L"FollowActor", 0));
    EXPECT(MoveSetting(L"RouteMove", 0));
    EXPECT(!MoveSetting(L"Stay", 0));
    EXPECT(!MoveSetting(L"Animation", 0));
    EXPECT(MoveSetting(L"AIStateGroup", kAiStateMoveTo));
    EXPECT(MoveSetting(L"aistategroup", kAiStateLead));
    EXPECT(!MoveSetting(L"AIStateGroup", kAiStateStay));
    EXPECT(!MoveSetting(L"Lea", 0));
    EXPECT(!MoveSetting(L"Stable", 0));
    EXPECT(!MoveSetting(L"AIStateGroup", kAiStateFollow));
    EXPECT(PathEnd(L"", 0));
    EXPECT(PathEnd(L"stable", 0));
    EXPECT(PathEnd(L"AIStateGroup", kAiStateFollow));  // a Follow group: the member follows again
    EXPECT(!PathEnd(L"AIStateGroup", kAiStateStay));
    EXPECT(!PathEnd(L"Stay", 0));
}

void TestVerdict()
{
    std::puts("verdict: any waiter, direct set or further move makes a guide; none is staging");
    EXPECT(VerdictOf({}) == Verdict::Staging);
    EXPECT(VerdictOf({.waiter = true}) == Verdict::Guide);
    EXPECT(VerdictOf({.directSets = true}) == Verdict::Guide);
    EXPECT(VerdictOf({.furtherMove = true}) == Verdict::Guide);
    EXPECT(VerdictOf({.waiter = true, .furtherMove = true}) == Verdict::Guide);
}

struct Row
{
    std::wstring_view status;
    std::uint8_t      groupState = 0;
    int               next       = -1;
};

bool Further(const Row* rows, int first, int maxLinks = 8)
{
    return FurtherMoveInChain(
        first, -1, [rows](int row) { return rows[row].next; }, [rows](int row) { return rows[row].status; },
        [rows](int row) { return rows[row].groupState; }, maxLinks);
}

void TestFurtherMoveInChain()
{
    std::puts("chain: a further move before the path's end is a path; a stand or the release ends it");
    // run, stay, run (Aerith's guide shape): from the stay, a further run.
    const std::array<Row, 3> path{{{L"MoveToTarget", 0, 1}, {L"Stay", 0, 2}, {L"MoveToTarget", 0, -1}}};
    EXPECT(Further(path.data(), 1));
    // run, then a stand: a placement.
    const std::array<Row, 2> placed{{{L"MoveToTarget", 0, 1}, {L"Stable", 0, -1}}};
    EXPECT(!Further(placed.data(), 1));
    // a stay, the release, then a later beat's run: not this path's.
    const std::array<Row, 3> later{{{L"Stay", 0, 1}, {L"", 0, 2}, {L"Lead", 0, -1}}};
    EXPECT(!Further(later.data(), 0));
    // a group row whose group runs.
    const std::array<Row, 2> group{{{L"Stay", 0, 1}, {L"AIStateGroup", kAiStateMoveTo, -1}}};
    EXPECT(Further(group.data(), 0));
    // a Follow group ends the path: the run after it is a later beat's.
    const std::array<Row, 3> follows{{{L"Stay", 0, 1}, {L"AIStateGroup", kAiStateFollow, 2}, {L"Lead", 0, -1}}};
    EXPECT(!Further(follows.data(), 0));
    // the first row itself a move; the first row a path end; no next row.
    EXPECT(Further(path.data(), 0));
    EXPECT(!Further(placed.data(), 1));
    EXPECT(!Further(path.data(), -1));
    // the bound: a move on the last link walked is found; a chain still going past the bound reads as a path (the
    // safe side: a guide), a loop of stays included; no links walked reads the same.
    const std::array<Row, 4> four{{{L"Stay", 0, 1}, {L"Stay", 0, 2}, {L"Stay", 0, 3}, {L"MoveToTarget", 0, -1}}};
    EXPECT(Further(four.data(), 0, 4));
    EXPECT(Further(four.data(), 0, 3));
    const std::array<Row, 3> loop{{{L"Stay", 0, 1}, {L"Stay", 0, 2}, {L"Stay", 0, 0}}};
    EXPECT(Further(loop.data(), 0, 8));
    EXPECT(Further(loop.data(), 0, 0));
    EXPECT(!Further(loop.data(), -1, 0));
}

void TestSameIgnoringCase()
{
    std::puts("names: ASCII letters without case, lengths must agree");
    EXPECT(SameIgnoringCase(L"MoveToTarget", L"MOVETOTARGET"));
    EXPECT(!SameIgnoringCase(L"Stay", L"Stays"));
    EXPECT(SameIgnoringCase(L"", L""));
}

}  // namespace

int main()
{
    TestHoldOf();
    TestMoveSettingAndPathEnd();
    TestVerdict();
    TestFurtherMoveInChain();
    TestSameIgnoringCase();

    return lss::test::Report();
}
