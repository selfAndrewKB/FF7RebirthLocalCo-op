// Host-side tests for hud::context_owners, run without the game.
//
// A frame's switches with three players: the roots shown in both further passes, in one, in the other and in none,
// each followed through player 3's pass, player 2's and the way back to player 1, counting the setter's calls.

#include "hud/context_owners.h"

#include "expect.h"

#include <array>
#include <cstdio>

namespace {

namespace owners = lss::hud::context_owners;

struct Root
{
    bool          neededByThree = false;
    bool          neededByTwo   = false;
    owners::Owner owner         = 0;
    int           calls         = 0;
};

void Pass(std::array<Root, 4>& roots, int seat)
{
    for (Root& root : roots)
    {
        const bool needed = seat == 2 ? root.neededByThree : root.neededByTwo;
        if (owners::SetForSeat(root.owner, seat, needed))
        {
            root.owner = static_cast<owners::Owner>(seat);
            ++root.calls;
        }
    }
}

void Back(std::array<Root, 4>& roots)
{
    for (Root& root : roots)
    {
        if (owners::SetForPlayerOne(root.owner))
        {
            root.owner = 0;
            ++root.calls;
        }
    }
}

void TestThreePlayerFrame()
{
    std::printf("three players: player 3's pass, player 2's, back to player 1\n");
    // [0] in both passes, [1] in player 3's only, [2] in player 2's only, [3] in neither (collapsed).
    std::array<Root, 4> roots{{{true, true}, {true, false}, {false, true}, {false, false}}};
    for (int frame = 0; frame < 2; ++frame)
    {
        Pass(roots, 2);  // seat 2 = player 3, the highest seat first
        EXPECT(roots[0].owner == 2 && roots[1].owner == 2 && roots[2].owner == 0 && roots[3].owner == 0);
        Pass(roots, 1);  // seat 1 = player 2: player 3's root not needed here keeps player 3's context
        EXPECT(roots[0].owner == 1 && roots[1].owner == 2 && roots[2].owner == 1 && roots[3].owner == 0);
        Back(roots);
        for (const Root& root : roots)
        {
            EXPECT(root.owner == 0);  // every root player 1's before player 1's pass
        }
    }
    EXPECT(roots[0].calls == 6);  // three a frame (was four with a return to player 1 between the passes)
    EXPECT(roots[1].calls == 4);
    EXPECT(roots[2].calls == 4);
    EXPECT(roots[3].calls == 0);
}

void TestTwoPlayerFrame()
{
    std::printf("two players: one pass and back, as before\n");
    std::array<Root, 4> roots{{{false, true}, {false, true}, {false, false}, {false, false}}};
    Pass(roots, 1);
    Back(roots);
    EXPECT(roots[0].calls == 2 && roots[1].calls == 2 && roots[2].calls == 0);
    EXPECT(roots[0].owner == 0 && roots[1].owner == 0);
}

void TestNoRepeatForTheSameSeat()
{
    std::printf("a root already the seat's is not set again\n");
    EXPECT(!owners::SetForSeat(1, 1, true));
    EXPECT(owners::SetForSeat(2, 1, true));
    EXPECT(!owners::SetForSeat(0, 1, false));  // not needed in the pass: left as it is
    EXPECT(!owners::SetForPlayerOne(0));
    EXPECT(owners::SetForPlayerOne(2));
}

}  // namespace

int main()
{
    TestThreePlayerFrame();
    TestTwoPlayerFrame();
    TestNoRepeatForTheSameSeat();

    return lss::test::Report();
}
