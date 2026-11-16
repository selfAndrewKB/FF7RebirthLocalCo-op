// Host-side tests for who the game warps at a battle's start and end (battle/battle_warps_rules.h).

#include "battle/battle_warps_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::battle_warps_rules;

void TestStays()
{
    std::puts("battle warps: off keeps players 2 and 3's characters, on keeps no one");
    EXPECT(!Stays(Mode::PlayersStay, 0));
    EXPECT(Stays(Mode::PlayersStay, 1));
    EXPECT(Stays(Mode::PlayersStay, 2));
    EXPECT(!Stays(Mode::Game, 0));
    EXPECT(!Stays(Mode::Game, 1));
    EXPECT(!Stays(Mode::Game, 2));
}

}  // namespace

int main()
{
    TestStays();

    return lss::test::Report();
}
