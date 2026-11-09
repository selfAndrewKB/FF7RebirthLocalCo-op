// Whom enemies follow while the game's leader is down (battle/enemy_leader_rules.h).

#include "battle/enemy_leader_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::enemy_leader_rules;

int               g_characters[4]{};  // distinct addresses for the characters
const void* const cloud  = &g_characters[0];
const void* const tifa   = &g_characters[1];
const void* const aerith = &g_characters[2];
const void* const barret = &g_characters[3];

void TestStandIn()
{
    std::puts("stand-in: the leader standing keeps it; else another player's standing character; else the switch's next "
              "standing member no player plays; else nobody");
    const Character seatsUp[2]   = {{tifa, 1, true}, {}};
    const Character seatsDown[2] = {{tifa, 1, false}, {}};
    // The battle list: Cloud (the leader, down), Tifa (player 2), Aerith (AI).
    const Character battle[3] = {{cloud, 0, false, true}, {tifa, 1, false, true}, {aerith, 2, true, false}};

    EXPECT(StandIn(true, seatsUp, battle, 0).character == nullptr);
    EXPECT(StandIn(false, seatsUp, battle, 0).character == tifa);
    const Character ally = StandIn(false, seatsDown, battle, 0);
    EXPECT(ally.character == aerith && ally.type == 2);
    // A standing player's character never becomes the AI stand-in, even when next in the list.
    const Character tifaUp[3] = {{cloud, 0, false, true}, {tifa, 1, true, true}, {aerith, 2, true, false}};
    EXPECT(StandIn(false, seatsDown, tifaUp, 0).character == aerith);
    // Nobody standing: nobody.
    const Character allDown[3] = {{cloud, 0, false, true}, {tifa, 1, false, true}, {aerith, 2, false, false}};
    EXPECT(StandIn(false, seatsDown, allDown, 0).character == nullptr);
    // The order wraps from the leader's place: the leader last in the list, the first AI member first.
    const Character wrapped[3] = {{barret, 3, true, false}, {aerith, 2, true, false}, {cloud, 0, false, true}};
    EXPECT(StandIn(false, seatsDown, wrapped, 2).character == barret);
    const Character fromMiddle[3] = {{barret, 3, true, false}, {cloud, 0, false, true}, {aerith, 2, true, false}};
    EXPECT(StandIn(false, seatsDown, fromMiddle, 1).character == aerith);
    // The leader not in the list: no AI stand-in.
    EXPECT(StandIn(false, seatsDown, battle, -1).character == nullptr);
    EXPECT(StandIn(false, seatsDown, battle, 3).character == nullptr);
    // Player 3's character when player 2's is down.
    const Character third[2] = {{tifa, 1, false}, {barret, 3, true}};
    EXPECT(StandIn(false, third, battle, 0).character == barret);
}

void TestTellsEnemies()
{
    std::puts("tells enemies: a change of the character they follow, not after the game's own switch, not back to a "
              "downed leader");
    EXPECT(TellsEnemies(true, false, true, false));    // the leader went down: to the stand-in
    EXPECT(TellsEnemies(true, false, false, true));    // the leader revived: back to it
    EXPECT(!TellsEnemies(true, false, false, false));  // the stand-in went down too: nobody to turn to
    EXPECT(!TellsEnemies(true, true, true, false));    // the game switched itself
    EXPECT(!TellsEnemies(false, false, true, false));
}

}  // namespace

int main()
{
    TestStandIn();
    TestTellsEnemies();
    return lss::test::Report();
}
