// Host-side tests for battle::lock_group_rules, run without the game.
//
// The rebalance's rule, and the set of held groups that reports only transitions.

#include "battle/lock_group_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::lock_group_rules;

/// The two-player rule as it stood before the seats, verbatim: the reference for the per-seat rule.
constexpr bool OldRebalanceHoldsPlayerTwo(bool playerTwoHoldsLock, bool playerOneHoldsLock) noexcept
{
    return playerTwoHoldsLock && !playerOneHoldsLock;
}

void TestRebalance()
{
    std::puts("lock group: the rebalance counts player 2 only while player 2 holds a lock and player 1 none");
    constexpr int kTwo = 1;
    EXPECT(RebalanceSeat(1u << kTwo, false) == kTwo);
    EXPECT(RebalanceSeat(1u << kTwo, true) == 0);
    EXPECT(RebalanceSeat(0, false) == 0);
    EXPECT(RebalanceSeat(0, true) == 0);
    // Side by side with the two-player rule over every input.
    for (int bits = 0; bits < 4; ++bits)
    {
        const bool two = (bits & 1) != 0, one = (bits & 2) != 0;
        EXPECT(OldRebalanceHoldsPlayerTwo(two, one) == (RebalanceSeat(two ? 1u << kTwo : 0u, one) == kTwo));
        EXPECT(OldRebalanceHoldsPlayerTwo(two, one) == (RebalanceSeat(two ? 1u << kTwo : 0u, one) != 0));
    }
}

void TestRebalanceThreePlayers()
{
    std::puts("lock group: three players, the first seat holding a lock while player 1 holds none");
    EXPECT(RebalanceSeat(0b100, false) == 2);
    EXPECT(RebalanceSeat(0b110, false) == 1);
    EXPECT(RebalanceSeat(0b110, true) == 0);
    EXPECT(RebalanceSeat(0b001, false) == 0);  // bit 0 is player 1's, never a seat
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
    TestRebalanceThreePlayers();
    TestHeldGroups();

    return lss::test::Report();
}
