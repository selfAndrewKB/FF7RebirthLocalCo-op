// Host-side tests for the switches players 2 and 3 may use (world/lever_rules.h), run without the game.

#include "world/lever_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::world::lever_rules;

void TestAdmitted()
{
    std::puts("admitted: the listed switches, by name without case; scenes, named characters and the script's stay out");
    EXPECT(Admitted(L"obl2200_Gmk_LiftSwitch_DOWN"));
    EXPECT(Admitted(L"OBL2200_GMK_LIFTSWITCH_down"));
    EXPECT(Admitted(L"obl3110_Switch_Elv210"));
    EXPECT(Admitted(L"obl2660_Switch_Elevator1_2_2"));
    EXPECT(!Admitted(L"obl2250_Lever_HydraulicPowerA"));     // starts a scene
    EXPECT(!Admitted(L"obl2250_Switch_LiftA_Upper"));        // names Yuffie
    EXPECT(!Admitted(L"obl2550_Lever_Lift"));                // the script pulls it with Cloud
    EXPECT(!Admitted(L"obl2200_TouchLift_WorldStory6_01"));  // unsettled: player 1's
    EXPECT(!Admitted(L"obl2660_Switch_Elevator1_2"));        // a prefix of a listed name is another switch
    EXPECT(!Admitted(L""));
    for (const auto name : kSwitches)
    {
        EXPECT(name.starts_with(L"obl"));
        EXPECT(Admitted(name));
    }
}

void TestLatch()
{
    std::puts("latch: down for every notify but the presser's; up for all while no press is pending");
    EXPECT(!LatchDownFor(-1, 0));
    EXPECT(!LatchDownFor(-1, 2));
    EXPECT(!LatchDownFor(1, 1));
    EXPECT(LatchDownFor(1, 0));
    EXPECT(LatchDownFor(0, 1));
    EXPECT(LatchDownFor(0, -1));  // a character no player plays never takes a player's press
    EXPECT(!LatchDownFor(0, 0));
}

}  // namespace

int main()
{
    TestAdmitted();
    TestLatch();
    return lss::test::Report();
}
