// Host-side tests for a player's ask to be brought back (world/party_warp_rules.h): the double click and "beside".

#include "world/party_warp_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::world::party_warp_rules;

void DoubleClicks()
{
    std::puts("party warp: two presses within 400 ms are a double click, the pair then spent");
    DoubleClick c;
    EXPECT(!c.Press(1000));
    EXPECT(c.Press(1300));   // the second within 400 ms
    EXPECT(!c.Press(1350));  // a third starts a new pair
    EXPECT(!c.Press(1800));  // too late for that one: a new pair again
    EXPECT(c.Press(2200));   // exactly 400 ms
    EXPECT(!c.Press(3000));
    EXPECT(!c.Press(3401));  // 401 ms: no
    static_assert(DoubleClick{1000}.Press(1400), "constexpr");
}

void BesideOrNot()
{
    std::puts("party warp: beside player 1's character in the plane and in height, or not");
    EXPECT(Beside(0.0f, 0.0f));
    EXPECT(Beside(400.0f, 0.0f));  // where the follow warp lands a follower
    EXPECT(Beside(499.0f, 149.0f) && Beside(499.0f, -149.0f));
    EXPECT(!Beside(500.0f, 0.0f));   // too far
    EXPECT(!Beside(100.0f, 150.0f));  // a floor above
    EXPECT(!Beside(100.0f, -150.0f));
}

}  // namespace

int main()
{
    DoubleClicks();
    BesideOrNot();
    return lss::test::Report();
}
