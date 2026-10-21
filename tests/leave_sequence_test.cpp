// Host-side tests for player_two::LeaveSequence, run without the game.
//
// The release always comes before the teardown, the teardown waits for the release to complete,
// and a release that never completes stops the leave instead of tearing down under it.

#include "player_two/leave_sequence.h"

#include "expect.h"

#include <cstdint>
#include <cstdio>

namespace {

using lss::player_two::LeaveAction;
using lss::player_two::LeaveSequence;

void TestWithoutMemberTearsDownAtOnce()
{
    std::puts("leave: with no member held, the teardown is the first step");
    LeaveSequence leave;
    EXPECT(leave.Begin(false) == LeaveAction::Teardown);
    EXPECT(leave.InProgress());
    leave.Finish();
    EXPECT(!leave.InProgress());
}

void TestReleaseComesFirstAndIsAwaited()
{
    std::puts("leave: release first, wait for it, then tear down");
    LeaveSequence leave;
    EXPECT(leave.Begin(true) == LeaveAction::Release);
    EXPECT(leave.Next(false) == LeaveAction::None);
    EXPECT(leave.Next(false) == LeaveAction::None);
    EXPECT(leave.Next(true) == LeaveAction::Teardown);
    EXPECT(leave.Next(true) == LeaveAction::Teardown);  // until the caller finishes it
    leave.Finish();
    EXPECT(leave.Next(true) == LeaveAction::None);
}

void TestSecondRequestDoesNotRestart()
{
    std::puts("leave: a second request while one is under way does not release again");
    LeaveSequence leave;
    EXPECT(leave.Begin(true) == LeaveAction::Release);
    EXPECT(leave.Begin(true) == LeaveAction::None);
    EXPECT(leave.Next(true) == LeaveAction::Teardown);
}

void TestStuckReleaseNeverTearsDown()
{
    std::puts("leave: a release that never completes is reported once and never torn down");
    LeaveSequence leave;
    EXPECT(leave.Begin(true) == LeaveAction::Release);
    std::uint32_t none = 0;
    for (std::uint32_t tick = 1; tick < LeaveSequence::kReleaseTicks; ++tick)
    {
        none += leave.Next(false) == LeaveAction::None ? 1 : 0;
    }
    EXPECT(none == LeaveSequence::kReleaseTicks - 1);
    EXPECT(leave.Next(false) == LeaveAction::ReportStuck);
    EXPECT(leave.Next(false) == LeaveAction::None);
    EXPECT(leave.Next(true) == LeaveAction::None);  // stuck until asked again
    EXPECT(leave.InProgress());

    std::puts("leave: asking again after a stuck release starts over with the release");
    EXPECT(leave.Begin(true) == LeaveAction::Release);
    EXPECT(leave.Next(true) == LeaveAction::Teardown);
}

}  // namespace

int main()
{
    TestWithoutMemberTearsDownAtOnce();
    TestReleaseComesFirstAndIsAwaited();
    TestSecondRequestDoesNotRestart();
    TestStuckReleaseNeverTearsDown();

    return lss::test::Report();
}
