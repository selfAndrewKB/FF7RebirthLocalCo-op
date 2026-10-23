// Host-side tests for player_two::world_rules, run without the game.
//
// The attack framing's latches and shot verdict, the location volume pick's rules, the move-type pair stack, the
// item widening's rules, the switch volume's enter, leave and player-1 changes, the camera repair's timer and verdict,
// the climb exits, the distance level and the shoulder scale.

#include "player_two/world/world_rules.h"

#include "expect.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace lss::player_two::world_rules;
using namespace std::chrono_literals;

int g_member, g_one, g_other;

struct Pair
{
    std::uint8_t move = 6, dash = 0;
    bool         operator==(const Pair&) const = default;
};

void TestLatches()
{
    std::puts("latches: a hit refreshes its use; a miss makes one; the least recently used goes when full");
    std::array<Latch, 3> latches{};
    bool                 made = false;
    Latch&               a    = LatchFor(latches, 0xA, 1, made);
    EXPECT(made && a.key == 0xA && a.value == 0 && a.used == 1);
    a.value = 7;
    LatchFor(latches, 0xB, 2, made);
    LatchFor(latches, 0xC, 3, made);
    Latch& again = LatchFor(latches, 0xA, 4, made);
    EXPECT(!made && &again == &a && again.value == 7 && again.used == 4);
    Latch& d = LatchFor(latches, 0xD, 5, made);  // full: 0xB (used 2) goes
    EXPECT(made && d.key == 0xD && &d == &latches[1]);
    LatchFor(latches, 0xB, 6, made);
    EXPECT(made);  // 0xB was evicted
}

void TestShot()
{
    std::puts("shot: a target that is no human's shows; the other player's target is off unless the camera is the viewer's");
    EXPECT(ShotShows(&g_other, &g_member, &g_one, false, &g_one));
    EXPECT(ShotShows(nullptr, &g_member, &g_one, true, nullptr));
    // Inside player 2's update: mine = member, other = player 1's.
    EXPECT(!ShotShows(&g_one, &g_member, &g_one, true, &g_one));     // player 1's target, their camera: off
    EXPECT(ShotShows(&g_one, &g_member, &g_one, true, &g_member));   // player 1's target on player 2's own camera
    EXPECT(ShotShows(&g_member, &g_member, &g_one, true, &g_one));   // player 2's own target
    // Player 1's update: mine = player 1's, other = member.
    EXPECT(!ShotShows(&g_member, &g_member, &g_one, false, &g_member));
    EXPECT(ShotShows(&g_member, &g_member, &g_one, false, &g_one));
    EXPECT(ShotShows(&g_one, &g_member, &g_one, false, &g_member));
}

void TestLocationAndStack()
{
    std::puts("locations: disabled or non-camera volumes skipped, the lowest priority wins, ties keep the first; the stack");
    EXPECT(LocationCandidate(0x2, 0x1, 0x2));
    EXPECT(!LocationCandidate(0x3, 0x1, 0x2));
    EXPECT(!LocationCandidate(0x0, 0x1, 0x2));
    EXPECT(LowerPriority(1, 2));
    EXPECT(!LowerPriority(2, 2));
    EXPECT(!LowerPriority(3, 2));

    PairStack<Pair, 4> stack;
    stack.Push({1, 2});
    stack.Push({3, 4});
    EXPECT((stack.Count() == 2 && stack.At(0).pair == Pair{3, 4} && stack.At(1).pair == Pair{1, 2}));
    stack.Push({1, 2});  // shared: a reference, no move
    EXPECT((stack.Count() == 2 && stack.At(1).refs == 2 && stack.At(0).pair == Pair{3, 4}));
    stack.Pop({1, 2});
    EXPECT(stack.Count() == 2 && stack.At(1).refs == 1);
    stack.Pop({3, 4});
    EXPECT((stack.Count() == 1 && stack.At(0).pair == Pair{1, 2}));
    stack.Pop({9, 9});  // not there
    EXPECT(stack.Count() == 1);
    stack.Pop({1, 2});
    EXPECT(stack.Count() == 0);

    EXPECT(ForcedInside(false, 1));
    EXPECT(ForcedInside(true, 0));
    EXPECT(!ForcedInside(true, 1));
}

void TestReach()
{
    std::puts("reach: whole steps past the distance; close within the radius plus the leave slack once widened; rewrites");
    EXPECT(WidenBy(0.0f, 500.0f, 1000.0f) == 1000.0f);
    EXPECT(WidenBy(500.0f, 500.0f, 1000.0f) == 1000.0f);
    EXPECT(WidenBy(501.0f, 500.0f, 1000.0f) == 2000.0f);
    EXPECT(WidenBy(2400.0f, 500.0f, 1000.0f) == 3000.0f);
    EXPECT(ItemClose(0, 3, 5, 100.0f, 100.0f, false, 500.0f));
    EXPECT(!ItemClose(0, 3, 5, 101.0f, 100.0f, false, 500.0f));
    EXPECT(ItemClose(0, 3, 5, 600.0f, 100.0f, true, 500.0f));
    EXPECT(!ItemClose(0, 3, 5, 601.0f, 100.0f, true, 500.0f));
    EXPECT(!ItemClose(3, 3, 5, 0.0f, 100.0f, false, 500.0f));
    EXPECT(!ItemClose(5, 3, 5, 0.0f, 100.0f, false, 500.0f));
    EXPECT(!Rewritten(1.0f, 2.0f, 1.0f, 2.0f));
    EXPECT(Rewritten(1.0f, 2.0f, 1.5f, 2.0f));
    EXPECT(Rewritten(1.0f, 2.0f, 1.0f, 2.5f));
}

void TestSwitchVolume()
{
    std::puts("switch volume: enters skipped by the on/off or an inside brush-only volume; leaves kept for player 1; "
              "player 1's changes");
    EXPECT(EnterVerdict(true, false, true, false) == Enter::SkippedOnOff);
    EXPECT(EnterVerdict(false, true, true, false) == Enter::SkippedOnOff);
    EXPECT(EnterVerdict(false, false, false, true) == Enter::AlreadyInside);
    EXPECT(EnterVerdict(false, false, true, true) == Enter::Call);  // counting: every enter counts
    EXPECT(EnterVerdict(false, false, false, false) == Enter::Call);

    EXPECT(LeaveVerdict(false, true, false, true) == Leave::NotEntered);
    EXPECT(LeaveVerdict(true, true, false, false) == Leave::SkippedOnOff);
    EXPECT(LeaveVerdict(true, false, false, true) == Leave::KeptForPlayerOne);
    EXPECT(LeaveVerdict(true, false, true, true) == Leave::Call);  // counting: player 1's own count is separate
    EXPECT(LeaveVerdict(true, false, false, false) == Leave::Call);

    EXPECT(PlayerOneChangeOf(true, false, true, true) == PlayerOneChange::Entered);
    EXPECT(PlayerOneChangeOf(false, true, false, false) == PlayerOneChange::StillInside);
    EXPECT(PlayerOneChangeOf(false, false, true, false) == PlayerOneChange::NotReentered);
    EXPECT(PlayerOneChangeOf(false, false, false, true) == PlayerOneChange::NotReentered);
    EXPECT(PlayerOneChangeOf(false, false, false, false) == PlayerOneChange::Reenter);
}

void TestRepair()
{
    std::puts("repair: finite for 2 s resets the tries; a repair only when due with tries left, then one give-up");
    HealthyTimer timer;
    const auto   t0 = HealthyTimer::Clock::time_point(10s);
    EXPECT(!timer.Observe(false, t0, 2s));
    EXPECT(!timer.Observe(true, t0, 2s));  // the first finite tick
    EXPECT(!timer.Observe(true, t0 + 1s, 2s));
    EXPECT(timer.Observe(true, t0 + 2s, 2s));
    EXPECT(timer.Observe(true, t0 + 3s, 2s));
    EXPECT(!timer.Observe(false, t0 + 3s, 2s));  // a NaN: the count starts over
    EXPECT(!timer.Observe(true, t0 + 4s, 2s));
    EXPECT(!timer.Observe(true, t0 + 5s, 2s));
    EXPECT(timer.Observe(true, t0 + 6s, 2s));

    EXPECT(RepairVerdict(true, false, true, true, 0, 5, false) == Repair::None);
    EXPECT(RepairVerdict(false, true, true, true, 0, 5, false) == Repair::None);
    EXPECT(RepairVerdict(false, false, false, true, 0, 5, false) == Repair::None);
    EXPECT(RepairVerdict(false, false, true, false, 0, 5, false) == Repair::None);
    EXPECT(RepairVerdict(false, false, true, true, 4, 5, false) == Repair::Repair);
    EXPECT(RepairVerdict(false, false, true, true, 5, 5, false) == Repair::GiveUp);
    EXPECT(RepairVerdict(false, false, true, true, 5, 5, true) == Repair::None);

    // The vectors' fourth lanes (+0x0C, +0x2C, +0x3C of the pose) are stack residue: not a camera value.
    EXPECT(PoseFinite(0));
    EXPECT(PoseFinite(1u << 15));             // the base point's lane alone (the buggy exit, 2026-10-21)
    EXPECT(PoseFinite(kPoseLanes));
    EXPECT(!PoseFinite(1u << 4));             // pitch (the chocobo warp, 2026-10-07)
    EXPECT(!PoseFinite((1u << 15) | (1u << 4)));
    EXPECT(!PoseFinite(1u << 16));            // distance
}

void TestTraversalAndCamera()
{
    std::puts("traversal: the climb exit phases and the ladder exit's wait; the distance level; the shoulder scale");
    for (const std::uint8_t state : {3, 6, 10, 11, 12})
    {
        EXPECT(ClimbExitState(state));
    }
    for (const std::uint8_t state : {0, 1, 2, 4, 5, 7, 8, 9, 13})
    {
        EXPECT(!ClimbExitState(state));
    }
    EXPECT(LadderExitWaits(true, false, false, 3, 4));
    EXPECT(!LadderExitWaits(true, false, false, 4, 4));
    EXPECT(!LadderExitWaits(false, false, false, 0, 4));
    EXPECT(!LadderExitWaits(true, true, false, 0, 4));
    EXPECT(!LadderExitWaits(true, false, true, 0, 4));

    EXPECT(DistanceLevel(5, 2, 1, 10) == 7);
    EXPECT(DistanceLevel(9, 5, 1, 10) == 10);
    EXPECT(DistanceLevel(1, -2, 1, 10) == 1);

    EXPECT(ShoulderScale(0.5f, 0.7f, 1.0f) == 0.7f);
    EXPECT(ShoulderScale(0.85f, 0.7f, 1.0f) == 0.85f);
    EXPECT(ShoulderScale(1.3f, 0.7f, 1.0f) == 1.0f);
}

}  // namespace

int main()
{
    TestLatches();
    TestShot();
    TestLocationAndStack();
    TestReach();
    TestSwitchVolume();
    TestRepair();
    TestTraversalAndCamera();

    return lss::test::Report();
}
