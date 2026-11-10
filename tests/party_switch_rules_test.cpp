// Host-side tests for session::party_switch_rules, run without the game.
//
// Whom each player's battle switch passes over, and when a request keeps the game's two-standing gate.

#include "session/party_switch_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::session::party_switch_rules;

void TestRefusals()
{
    std::puts("switch refusals: player 1 passes over every seat's member; a seat passes over player 1's character and "
              "the other seats' members, never its own");
    // Player 1's switch (no pass in flight).
    EXPECT(!SwitchRefuses(0, 0, false));  // an AI member
    EXPECT(SwitchRefuses(0, 1, false));   // player 2's member
    EXPECT(SwitchRefuses(0, 2, false));   // player 3's member
    EXPECT(!SwitchRefuses(0, 0, true));   // player 1's own character: the cycle's start, counted as the leader

    // Player 2's pass.
    EXPECT(!SwitchRefuses(1, 0, false));  // an AI member
    EXPECT(SwitchRefuses(1, 0, true));    // player 1's character
    EXPECT(!SwitchRefuses(1, 1, false));  // player 2's own member: the cycle's start
    EXPECT(SwitchRefuses(1, 2, false));   // player 3's member

    // Player 3's pass.
    EXPECT(SwitchRefuses(2, 1, false));
    EXPECT(!SwitchRefuses(2, 2, false));
    EXPECT(SwitchRefuses(2, 0, true));
}

void TestCanSwitchGate()
{
    std::puts("the two-standing gate: kept as the game asks, dropped only in co-op for a leader that cannot act");
    EXPECT(RequiresCanSwitch(true, false, false));
    EXPECT(RequiresCanSwitch(true, false, true));   // one player: the game's own rule (its forced switch runs there)
    EXPECT(RequiresCanSwitch(true, true, false));   // co-op, the leader standing
    EXPECT(!RequiresCanSwitch(true, true, true));   // co-op, the leader down: dropped
    EXPECT(!RequiresCanSwitch(false, true, false)); // the game did not ask for it (a forced switch): never added
    EXPECT(!RequiresCanSwitch(false, false, true));
}

}  // namespace

int main()
{
    TestRefusals();
    TestCanSwitchGate();

    return lss::test::Report();
}
