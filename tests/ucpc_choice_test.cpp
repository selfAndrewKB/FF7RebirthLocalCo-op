// Host-side tests for player_two::ucpc_choice, run without the game.
//
// Player 2 not in: untouched. Both eligible: the coin decides. One eligible: that one, whatever the coin. Neither:
// player 1, as the game would.

#include "player_two/ucpc_choice.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::player_two::ucpc_choice;

constexpr bool kCoins[] = {false, true};

bool Is(Choice choice, Ucpc ucpc, Why why)
{
    return choice.ucpc == ucpc && choice.why == why;
}

void TestNoPlayerTwo()
{
    std::puts("ucpc choice: without player 2 the game's answer is untouched");
    for (int bits = 0; bits < 8; ++bits)
    {
        EXPECT(Is(Choose(false, bits & 1, bits & 2, bits & 4), Ucpc::Untouched, Why::NoPlayerTwo));
    }
}

void TestBothEligible()
{
    std::puts("ucpc choice: both eligible, the coin decides");
    EXPECT(Is(Choose(true, true, true, false), Ucpc::PlayerOne, Why::Coin));
    EXPECT(Is(Choose(true, true, true, true), Ucpc::PlayerTwo, Why::Coin));
}

void TestOneEligible()
{
    std::puts("ucpc choice: one eligible takes it, whatever the coin");
    for (const bool coin : kCoins)
    {
        EXPECT(Is(Choose(true, true, false, coin), Ucpc::PlayerOne, Why::OnlyEligible));
        EXPECT(Is(Choose(true, false, true, coin), Ucpc::PlayerTwo, Why::OnlyEligible));
    }
}

void TestNeitherEligible()
{
    std::puts("ucpc choice: neither eligible, player 1 as the game would");
    for (const bool coin : kCoins)
    {
        EXPECT(Is(Choose(true, false, false, coin), Ucpc::PlayerOne, Why::NeitherEligible));
    }
}

static_assert(Choose(true, true, true, true).ucpc == Ucpc::PlayerTwo);

}  // namespace

int main()
{
    TestNoPlayerTwo();
    TestBothEligible();
    TestOneEligible();
    TestNeitherEligible();
    return lss::test::Report();
}
