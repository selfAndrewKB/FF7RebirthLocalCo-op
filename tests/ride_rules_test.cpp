// Host-side tests for rides::ride_rules, run without the game.
//
// The chocobo ride's end retry, seated verdict, end reasons in precedence order and getoff rule; the wheelie's forced
// getoff plan, coordinator transitions and world stick; the shared ride's riding and change rules; the end texts per
// seat.

#include "rides/ride_rules.h"

#include "expect.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace lss::rides::ride_rules;

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

void TestChocoboEnd()
{
    std::puts("chocobo: the end retries 60 ticks unless forced; the reasons in order: gone, dismounted, battle, the "
              "manager, detached, possession");
    std::uint32_t tries = 0;
    for (int i = 1; i < 60; ++i)
    {
        EXPECT(EndRetryAllowed(false, tries, 60));
    }
    EXPECT(!EndRetryAllowed(false, tries, 60));
    EXPECT(tries == 60);
    tries = 0;
    EXPECT(!EndRetryAllowed(true, tries, 60));
    EXPECT(tries == 0);  // a forced end does not count

    EndReads reads;
    EXPECT(EndReasonOf(reads) == EndReason::None);
    EXPECT(Describe(EndReason::None) == nullptr);
    reads.holds = false;
    EXPECT(EndReasonOf(reads) == EndReason::LostPossession);
    reads.attached = false;
    EXPECT(EndReasonOf(reads) == EndReason::Detached);
    reads.entryRiding = false;
    EXPECT(EndReasonOf(reads) == EndReason::ManagerEnded);
    reads.inBattle = true;
    EXPECT(EndReasonOf(reads) == EndReason::Battle);
    reads.ridden = false;
    EXPECT(EndReasonOf(reads) == EndReason::Dismounted);
    reads.gone = true;
    EXPECT(EndReasonOf(reads) == EndReason::Gone);
    for (const auto reason : {EndReason::Gone, EndReason::Dismounted, EndReason::Battle, EndReason::ManagerEnded,
                              EndReason::Detached, EndReason::LostPossession})
    {
        EXPECT(Describe(reason) != nullptr);
    }
    EXPECT(std::wstring(Describe(EndReason::Dismounted)) == L"the game dismounted the party");

    EXPECT(!GetOffDone(true, false, false, false));
    EXPECT(GetOffDone(false, false, false, false));
    EXPECT(GetOffDone(true, true, false, false));
    EXPECT(GetOffDone(true, false, true, false));
    EXPECT(GetOffDone(true, false, false, true));
}

void TestSeated()
{
    std::puts("chocobo: seated only when ridden, not held back, attached and out of battle");
    EXPECT(SeatedVerdict(true, false, true, false) == Seated::Seated);
    EXPECT(SeatedVerdict(false, true, true, false) == Seated::Unridden);  // unridden first: clears the hold-back
    EXPECT(SeatedVerdict(true, true, false, true) == Seated::HeldBack);
    EXPECT(SeatedVerdict(true, false, false, true) == Seated::Detached);
    EXPECT(SeatedVerdict(true, false, true, true) == Seated::Battle);
}

void TestWheelie()
{
    std::puts("wheelie: a forced getoff is followed for type 0 or the scooter's while riding, instant when player 1 is "
              "far; the coordinator's idle finishes, a negative state is gone; the world stick");
    int        farReads = 0;
    const auto far      = [&] { ++farReads; return true; };
    const auto near     = [&] { ++farReads; return false; };
    EXPECT(ForcedGetOffPlanOf(0, 3, 4, 3, false, near) == ForcedGetOff::Follow);
    EXPECT(ForcedGetOffPlanOf(4, 3, 4, 3, false, far) == ForcedGetOff::FollowInstant);
    EXPECT(ForcedGetOffPlanOf(4, 3, 4, 3, true, far) == ForcedGetOff::Follow);  // instant already: not read
    EXPECT(farReads == 2);
    EXPECT(ForcedGetOffPlanOf(7, 3, 4, 3, false, far) == ForcedGetOff::Skip);  // another vehicle's getoff
    EXPECT(ForcedGetOffPlanOf(0, 2, 4, 3, false, far) == ForcedGetOff::Skip);  // boarding, not riding
    EXPECT(farReads == 2);

    EXPECT(CoordChangeOf(3, 3, 0) == CoordChange::None);
    EXPECT(CoordChangeOf(3, 4, 0) == CoordChange::None);
    EXPECT(CoordChangeOf(4, 0, 0) == CoordChange::Finished);
    EXPECT(CoordChangeOf(3, -1, 0) == CoordChange::Gone);
    EXPECT(CoordChangeOf(0, 0, 0) == CoordChange::None);

    const float zero[3] = {0, 1e-5f, -1e-5f};
    const float moves[3] = {0, 0, 2e-4f};
    EXPECT(!RequestMoves(zero, 1e-4f));
    EXPECT(RequestMoves(moves, 1e-4f));

    auto stick = WorldStickOf(0, 1, 0);  // forward, camera at yaw 0: +x
    EXPECT(Near(stick[0], 1) && Near(stick[1], 0));
    stick = WorldStickOf(0, 1, 90);  // forward at yaw 90: +y
    EXPECT(Near(stick[0], 0) && Near(stick[1], 1));
    stick = WorldStickOf(1, 0, 0);  // right at yaw 0: +y (the game's axes: x forward, y to the right)
    EXPECT(Near(stick[0], 0) && Near(stick[1], 1));
    stick = WorldStickOf(1, 0, 90);  // right at yaw 90: -x
    EXPECT(Near(stick[0], -1) && Near(stick[1], 0));
}

void TestSharedRide()
{
    std::puts("shared ride: a known coordinator not idle; a bronco coordinator at state 0 is no ride; starts, ends and "
              "switches");
    EXPECT(SharedRiding(true, 3, 0));
    EXPECT(!SharedRiding(true, 0, 0));
    EXPECT(!SharedRiding(false, 3, 0));
    EXPECT(!RideChanged(false, 0, false, 1));
    EXPECT(RideChanged(false, 0, true, 0));
    EXPECT(RideChanged(true, 0, false, 0));
    EXPECT(!RideChanged(true, 1, true, 1));
    EXPECT(RideChanged(true, 0, true, 1));
}

void TestEndTextPerSeat()
{
    std::puts("chocobo: the end texts per seat: player 2's as before, the lost possession naming the seat's controller");
    for (const auto reason : {EndReason::None, EndReason::Gone, EndReason::Dismounted, EndReason::Battle,
                              EndReason::ManagerEnded, EndReason::Detached, EndReason::LostPossession})
    {
        EXPECT(Describe(reason, lss::players::kFirstSeat) == Describe(reason));  // the same literal for player 2
        if (reason != EndReason::LostPossession)
        {
            EXPECT(Describe(reason, 2) == Describe(reason));
        }
    }
    EXPECT(std::wstring(Describe(EndReason::LostPossession, 2)) ==
           L"player 3's controller no longer holds the chocobo");
    EXPECT(std::wstring(Describe(EndReason::LostPossession, 7)) ==
           L"the rider's controller no longer holds the chocobo");
    static_assert(Describe(EndReason::Gone, 2) != nullptr);
}

}  // namespace

int main()
{
    TestChocoboEnd();
    TestSeated();
    TestWheelie();
    TestSharedRide();
    TestEndTextPerSeat();

    return lss::test::Report();
}
