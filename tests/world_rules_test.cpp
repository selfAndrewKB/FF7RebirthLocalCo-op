// Host-side tests for player_two::world_rules, run without the game.
//
// The attack framing's latches and shot verdict, the location volume pick's rules, the move-type pair stack, the
// item widening's rules, the switch volume's enter, leave and player-1 changes, the camera repair's timer and verdict,
// the climb exits, the distance level and the shoulder scale.

#include "player_two/world/world_rules.h"

#include "expect.h"

#include <chrono>
#include <cstdio>
#include <initializer_list>

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

/// ShotShows as it was before the seats (v0.91.0), the reference the per-seat form must match for two players.
constexpr bool ShotShowsTwoPlayer(const void* target, const void* member, const void* one, bool insidePlayerTwo,
                                  const void* owner) noexcept
{
    if (target != member && target != one)
    {
        return true;
    }
    const void* mine  = insidePlayerTwo ? member : one;
    const void* other = insidePlayerTwo ? one : member;
    return !(target == other && owner != mine);
}

void TestShotSeats()
{
    std::puts("shot by seat: two players answer as before for every non-null target; a third player's target is off "
              "on the other cameras unless the shot is the viewer's");
    int         three = 0;
    const void* targets[] = {&g_other, &g_one, &g_member, &three};
    const void* members[] = {&g_member, nullptr};
    const void* ones[]    = {&g_one, nullptr};
    const void* owners[]  = {nullptr, &g_one, &g_member, &g_other, &three};
    for (const void* target : targets)
    {
        for (const void* member : members)
        {
            for (const void* one : ones)
            {
                for (const bool inside : {false, true})
                {
                    for (const void* owner : owners)
                    {
                        EXPECT(ShotShows(target, member, one, inside, owner) ==
                               ShotShowsTwoPlayer(target, member, one, inside, owner));
                    }
                }
            }
        }
    }
    const void* const humans[lss::players::kMaxPlayers] = {&g_one, &g_member, &three};
    EXPECT(ShotShows(&g_other, humans, 2, nullptr));       // no human's target: every camera
    EXPECT(ShotShows(&three, humans, 2, &g_one));        // player 3's own target on player 3's camera
    EXPECT(!ShotShows(&three, humans, 0, &three));     // player 3's target on player 1's camera: off
    EXPECT(!ShotShows(&three, humans, 1, &g_one));       // ... and on player 2's, the shot not player 2's
    EXPECT(ShotShows(&three, humans, 1, &g_member));     // ... unless the shot is player 2's own
    EXPECT(!ShotShows(&g_member, humans, 2, &g_member));   // player 2's target on player 3's camera: off
    EXPECT(ShotShows(&g_one, humans, 2, &three));        // player 1's target in player 3's own shot
    // One pointer for both characters (never in play): the documented difference, player 1's own camera shows it.
    EXPECT(!ShotShowsTwoPlayer(&g_one, &g_one, &g_one, false, &g_other));
    EXPECT(ShotShows(&g_one, &g_one, &g_one, false, &g_other));
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

void TestSwitchVolumeSeats()
{
    std::puts("switch volume by seat: one seat answers as before; a second seat is noted without a call, a leave is "
              "kept while another seat is inside, a gone seat calls nothing while another player is inside, the "
              "re-enter takes the lowest seat");
    const std::uint8_t one  = VolumeSeatBit(1);
    const std::uint8_t two  = VolumeSeatBit(2);
    const std::uint8_t both = static_cast<std::uint8_t>(one | two);
    // One seat (seat 1): the same answers as the two-player rules, with `held` = seat 1 inside.
    for (const bool box : {false, true})
    {
        for (const bool inOut : {false, true})
        {
            for (const bool counts : {false, true})
            {
                for (const bool held : {false, true})
                {
                    const std::uint8_t mask = held ? one : std::uint8_t{0};
                    EXPECT(SeatEnterVerdict(box, inOut, counts, mask) == EnterVerdict(box, inOut, counts, held));
                    for (const bool p1 : {false, true})
                    {
                        EXPECT(SeatLeaveVerdict(mask, 1, inOut, counts, p1) ==
                               LeaveVerdict(held, inOut, held && counts, held && p1));
                        if (held)
                        {
                            EXPECT(LeaveAllCalls(counts, 3, mask, 1, p1) == (counts ? 3 : (p1 ? 0 : 1)));
                        }
                    }
                }
            }
        }
    }
    EXPECT(ReenterSeat(one) == 1);
    EXPECT(ReenterSeat(0) == -1);
    // Two seats in a brush-only volume.
    EXPECT(SeatEnterVerdict(false, false, false, one) == Enter::AlreadyInside);  // seat 2 noted, no call
    EXPECT(SeatEnterVerdict(false, false, true, one) == Enter::Call);            // a counting volume counts it
    EXPECT(OtherInside(both, 1, false) && OtherInside(both, 2, false));
    EXPECT(!OtherInside(two, 2, false) && OtherInside(two, 2, true));
    EXPECT(SeatLeaveVerdict(both, 1, false, false, false) == Leave::KeptForPlayerOne);  // seat 2 still inside
    EXPECT(SeatLeaveVerdict(two, 2, false, false, false) == Leave::Call);              // the last one out
    EXPECT(SeatLeaveVerdict(two, 1, false, false, false) == Leave::NotEntered);
    EXPECT(SeatLeaveVerdict(both, 1, false, true, false) == Leave::Call);  // counting: the game's count decides
    EXPECT(LeaveAllCalls(false, 1, both, 2, false) == 0);  // seat 2 gone, seat 1 inside: no call
    EXPECT(LeaveAllCalls(false, 1, two, 2, true) == 0);    // player 1 inside: no call
    EXPECT(LeaveAllCalls(false, 1, two, 2, false) == 1);
    EXPECT(LeaveAllCalls(true, 2, both, 2, true) == 2);    // counting: its own enters
    EXPECT(ReenterSeat(both) == 1 && ReenterSeat(two) == 2);
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
    TestShotSeats();
    TestLocationAndStack();
    TestReach();
    TestSwitchVolume();
    TestSwitchVolumeSeats();
    TestRepair();
    TestTraversalAndCamera();

    return lss::test::Report();
}
