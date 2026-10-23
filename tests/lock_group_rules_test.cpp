// Host-side tests for player_two::lock_group_rules, run without the game.
//
// The rebalance's rule, and the set of held groups that reports only transitions.

#include "player_two/battle/lock_group_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::player_two::lock_group_rules;

void TestRebalance()
{
    std::puts("lock group: the rebalance counts player 2 only while player 2 holds a lock and player 1 none");
    EXPECT(RebalanceHoldsPlayerTwo(true, false));
    EXPECT(!RebalanceHoldsPlayerTwo(true, true));
    EXPECT(!RebalanceHoldsPlayerTwo(false, false));
    EXPECT(!RebalanceHoldsPlayerTwo(false, true));
}

void TestHeldGroups()
{
    std::puts("held groups: a change is reported on a transition only; a fifth group is not recorded");
    int           a, b, c, d, e;
    HeldGroups<4> held;
    EXPECT(!held.Note(&a, false));  // not held, not recorded: nothing changed
    EXPECT(held.Note(&a, true));
    EXPECT(!held.Note(&a, true));  // still held
    EXPECT(held.Groups()[0] == &a);
    EXPECT(held.Note(&a, false));
    EXPECT(!held.Note(&a, false));
    EXPECT(held.Groups()[0] == nullptr);

    EXPECT(held.Note(&a, true) && held.Note(&b, true) && held.Note(&c, true) && held.Note(&d, true));
    EXPECT(!held.Note(&e, true));  // full: answered, unrecorded
    EXPECT(!held.Note(&e, false));
    EXPECT(held.Note(&b, false));  // frees a slot
    EXPECT(held.Note(&e, true));
    EXPECT(held.Groups()[1] == &e);

    held.Clear();
    for (const void* group : held.Groups())
    {
        EXPECT(group == nullptr);
    }
}

}  // namespace

int main()
{
    TestRebalance();
    TestHeldGroups();

    return lss::test::Report();
}
