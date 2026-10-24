// Host-side tests for the walk-cap numbering in player_two/input/move_type_two.h: player numbers (1 player 1, seat + 1
// for a seat, 0 another controller) and their bits in the set of controllers to clear, which the two-player mod wrote
// as `1 << player` with player 1 and 2.

#include "player_two/input/move_type_two.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::player_two::move_type_two;
namespace players = lss::players;

void TestTwoPlayerNumbersUnchanged()
{
    std::puts("walk cap: player 1 is number 1 and player 2 number 2, their bits 1 << 1 and 1 << 2 as before");
    static_assert(PlayerNumber(0) == 1, "player 1");
    static_assert(PlayerNumber(players::kPlayerTwoSeat) == 2, "player 2");
    EXPECT(ClearBit(1) == (1 << 1));
    EXPECT(ClearBit(2) == (1 << 2));
}

void TestRoundTrip()
{
    std::puts("walk cap: every player's number names its seat back, and the bits are apart");
    int seen = 0;
    for (int seat = 0; seat < players::kMaxPlayers; ++seat)
    {
        const int player = PlayerNumber(seat);
        EXPECT(SeatOfPlayerNumber(player) == seat);
        EXPECT(ClearBit(player) != 0 && (seen & ClearBit(player)) == 0);
        seen |= ClearBit(player);
    }
    EXPECT(ClearBit(3) == (1 << 3));  // player 3
}

void TestAnotherController()
{
    std::puts("walk cap: 0 (another controller) and numbers past the players name no seat and no bit");
    EXPECT(SeatOfPlayerNumber(0) == -1 && ClearBit(0) == 0);
    EXPECT(SeatOfPlayerNumber(players::kMaxPlayers + 1) == -1 && ClearBit(players::kMaxPlayers + 1) == 0);
    EXPECT(SeatOfPlayerNumber(-1) == -1 && ClearBit(-1) == 0);
}

}  // namespace

int main()
{
    TestTwoPlayerNumbersUnchanged();
    TestRoundTrip();
    TestAnotherController();
    return lss::test::Report();
}
