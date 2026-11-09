// Host-side tests for the held member's script rule (session/field_hold.h), run without the game.

#include "session/field_hold.h"

#include "expect.h"

#include <cstdio>

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
    std::puts("settings: the moving Status words without case, a group when it moves; the path's end");
    EXPECT(MoveSetting(L"Lead", false));
    EXPECT(MoveSetting(L"movetotarget", false));
    EXPECT(MoveSetting(L"FollowActor", false));
    EXPECT(MoveSetting(L"RouteMove", false));
    EXPECT(!MoveSetting(L"Stay", true));  // the group answer counts only for a group row
    EXPECT(!MoveSetting(L"Animation", false));
    EXPECT(MoveSetting(L"AIStateGroup", true));
    EXPECT(MoveSetting(L"aistategroup", true));
    EXPECT(!MoveSetting(L"AIStateGroup", false));
    EXPECT(!MoveSetting(L"Lea", false));
    EXPECT(!MoveSetting(L"Stable", false));
    EXPECT(PathEnd(L"", false));
    EXPECT(PathEnd(L"stable", false));
    EXPECT(PathEnd(L"AIStateGroup", true));  // a Follow group: the member follows again
    EXPECT(!PathEnd(L"AIStateGroup", false));
    EXPECT(!PathEnd(L"Stay", true));
}

void TestGroups()
{
    std::puts("groups: one run or lead naming a trigger moves the member, whatever comes first; FOLLOW alone follows");
    // The resident groups (RE-walk-gate.md section 2.3): LeadDefault opens with a stay; a Stay group's walk back to its
    // anchor names no trigger; StayLookRelay's walk does (the setting row's argument 8); MoveToAnimation animates first.
    const GroupState leadDefault[] = {{kAiStateStay, -1}, {kAiStateLead, 2}, {kAiStateStay, -1}};
    const GroupState stayDefault[] = {{kAiStateStay, -1}, {kAiStateMoveTo, -1}};
    const GroupState stayRelay[]   = {{kAiStateStay, -1}, {kAiStateMoveTo, 8}};
    const GroupState moveAnim[]    = {{kAiStateAnimation, -1}, {kAiStateMoveTo, 2}};
    const GroupState follow[]      = {{kAiStateFollow, -1}};
    const GroupState mixed[]       = {{kAiStateFollow, -1}, {kAiStateStay, -1}};
    EXPECT(GroupMoves(leadDefault, 3));
    EXPECT(!GroupMoves(stayDefault, 2));
    EXPECT(GroupMoves(stayRelay, 2));
    EXPECT(GroupMoves(moveAnim, 2));
    EXPECT(!GroupMoves(follow, 1));
    EXPECT(!GroupMoves(nullptr, 0));
    EXPECT(GroupFollows(follow, 1));
    EXPECT(!GroupFollows(mixed, 2));
    EXPECT(!GroupFollows(leadDefault, 3));
    EXPECT(!GroupFollows(nullptr, 0));  // an unknown group follows nothing
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
    TestGroups();
    TestSameIgnoringCase();

    return lss::test::Report();
}
