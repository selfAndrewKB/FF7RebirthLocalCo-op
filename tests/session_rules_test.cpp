// Host-side tests for session::session_rules, run without the game.
//
// The holds of the session's Playing phase, which of them retake a member, and what a game leader change asks of
// the session.

#include "session/session_rules.h"

#include "expect.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace lss::session::session_rules;

int g_member;
int g_other;

void TestHolds()
{
    std::puts("holds: withdrawn first; a member is played or faulted; without one, standing aside, the slot wait or orphaned");
    EXPECT(HoldOf(&g_member, false, false, &g_member, true) == Hold::Member);
    EXPECT(HoldOf(&g_member, true, true, &g_member, true) == Hold::Member);     // the member rules over the flags
    EXPECT(HoldOf(&g_member, false, false, nullptr, true) == Hold::Faulted);    // the take timed out: still recorded
    EXPECT(HoldOf(&g_member, false, false, &g_other, true) == Hold::Faulted);
    EXPECT(HoldOf(nullptr, false, true, nullptr, true) == Hold::StandAside);
    EXPECT(HoldOf(nullptr, true, true, nullptr, true) == Hold::StandAside);     // the ride rules over the slot wait
    EXPECT(HoldOf(nullptr, true, false, nullptr, true) == Hold::SlotWait);
    EXPECT(HoldOf(nullptr, false, false, nullptr, true) == Hold::Orphaned);
    EXPECT(HoldOf(nullptr, false, false, nullptr, false) == Hold::Withdrawn);
    EXPECT(HoldOf(&g_member, false, false, &g_member, false) == Hold::Withdrawn);  // withdrawn rules over everything

    EXPECT(MayRetake(Hold::SlotWait));
    EXPECT(MayRetake(Hold::Orphaned));
    EXPECT(!MayRetake(Hold::Member));
    EXPECT(!MayRetake(Hold::Faulted));
    EXPECT(!MayRetake(Hold::StandAside));
    EXPECT(!MayRetake(Hold::Withdrawn));
}

void TestLeaderEvents()
{
    std::puts("leader change: out and waiting arms a look; player 2's controller only warns; player 2's held member about "
              "to be player 1's yields; without a member, a look");
    LeaderEvent out;
    EXPECT(DecideLeaderEvent(out) == LeaderEventAction::None);
    out.awaitingMember = true;
    EXPECT(DecideLeaderEvent(out) == LeaderEventAction::ArmLook);
    out.playerTwosController = true;  // out: no controller is player 2's, whatever the flags say
    EXPECT(DecideLeaderEvent(out) == LeaderEventAction::ArmLook);

    LeaderEvent in;
    in.hasPlayerTwo = true;
    EXPECT(DecideLeaderEvent(in) == LeaderEventAction::None);

    auto twos                 = in;
    twos.playerTwosController = true;
    twos.playerTwosMember     = true;
    twos.memberHeld           = true;
    EXPECT(DecideLeaderEvent(twos) == LeaderEventAction::Warn);  // never acted on

    auto yield                 = in;
    yield.playerOnesController = true;
    yield.playerTwosMember     = true;
    yield.memberHeld           = true;
    EXPECT(DecideLeaderEvent(yield) == LeaderEventAction::Yield);
    auto notHeld       = yield;
    notHeld.memberHeld = false;  // a swap in flight: the member is recorded, not held
    EXPECT(DecideLeaderEvent(notHeld) == LeaderEventAction::None);
    auto another                 = yield;
    another.playerOnesController = false;  // a controller that is not player 1's
    EXPECT(DecideLeaderEvent(another) == LeaderEventAction::None);
    auto othersCharacter             = yield;
    othersCharacter.playerTwosMember = false;
    EXPECT(DecideLeaderEvent(othersCharacter) == LeaderEventAction::None);

    auto none     = in;
    none.noMember = true;
    EXPECT(DecideLeaderEvent(none) == LeaderEventAction::ArmLook);
    none.playerOnesController = true;
    EXPECT(DecideLeaderEvent(none) == LeaderEventAction::ArmLook);
}

void TestWaitOutside()
{
    std::puts("wait outside: any slot wait on one monitor past the grace; never on the row or for another hold");
    using std::chrono::milliseconds;
    const milliseconds     grace{1500};
    const WaitOutsideFacts leaves{.hold = Hold::SlotWait, .columnsHeld = false, .waited = milliseconds{1500}};
    EXPECT(WaitOutside(leaves, grace));
    WaitOutsideFacts f = leaves;
    f.waited           = milliseconds{9000};
    EXPECT(WaitOutside(f, grace));
    f.waited = milliseconds{1499};  // a party swap resolving within a second
    EXPECT(!WaitOutside(f, grace));
    f.waited = milliseconds{0};
    EXPECT(!WaitOutside(f, grace));

    f             = leaves;
    f.columnsHeld = true;  // the row: the column stays
    EXPECT(!WaitOutside(f, grace));
    for (const auto hold : {Hold::StandAside, Hold::Orphaned, Hold::Member, Hold::Faulted, Hold::Withdrawn})
    {
        f      = leaves;
        f.hold = hold;  // a shared ride never leaves; the retake's own leave decides for an orphaned seat
        EXPECT(!WaitOutside(f, grace));
    }
}

}  // namespace

int main()
{
    TestHolds();
    TestLeaderEvents();
    TestWaitOutside();

    return lss::test::Report();
}
