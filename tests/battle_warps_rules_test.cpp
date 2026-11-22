// Host-side tests for who the game warps at a battle's start and end (battle/battle_warps_rules.h).

#include "battle/battle_warps_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::battle_warps_rules;

void TestStays()
{
    std::puts("battle warps: off keeps players 2 and 3's characters, on keeps no one");
    EXPECT(!Stays(Mode::PlayersStay, -1));
    EXPECT(!Stays(Mode::PlayersStay, 0));
    EXPECT(Stays(Mode::PlayersStay, 1));
    EXPECT(Stays(Mode::PlayersStay, 2));
    EXPECT(!Stays(Mode::Game, -1));
    EXPECT(!Stays(Mode::Game, 0));
    EXPECT(!Stays(Mode::Game, 1));
    EXPECT(!Stays(Mode::Game, 2));
}

void TestNoticedAnchor()
{
    std::puts("battle warps: a fight that noticed player 2 gathers at them; off keeps player 1 and 3, on keeps no one");
    EXPECT(Stays(Mode::PlayersStay, 0, 1));
    EXPECT(!Stays(Mode::PlayersStay, 1, 1));
    EXPECT(Stays(Mode::PlayersStay, 2, 1));
    EXPECT(!Stays(Mode::PlayersStay, -1, 1));
    EXPECT(!Stays(Mode::Game, 0, 1));
    EXPECT(!Stays(Mode::Game, 2, 1));
}

}  // namespace

int main()
{
    TestStays();
    TestNoticedAnchor();

    return lss::test::Report();
}
