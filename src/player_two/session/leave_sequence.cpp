#include "player_two/session/leave_sequence.h"

namespace lss::player_two {

LeaveAction LeaveSequence::Begin(bool holdsMember) noexcept
{
    if (phase_ == Phase::AwaitingRelease || phase_ == Phase::ReadyToTeardown)
    {
        return LeaveAction::None;
    }
    // From Stuck, a new request starts again: the release is idempotent once it has taken effect.
    waited_ = 0;
    if (!holdsMember)
    {
        phase_ = Phase::ReadyToTeardown;
        return LeaveAction::Teardown;
    }
    phase_ = Phase::AwaitingRelease;
    return LeaveAction::Release;
}

LeaveAction LeaveSequence::Next(bool released) noexcept
{
    switch (phase_)
    {
        case Phase::Idle:
        case Phase::Stuck: return LeaveAction::None;
        case Phase::ReadyToTeardown: return LeaveAction::Teardown;
        case Phase::AwaitingRelease: break;
    }
    if (released)
    {
        phase_ = Phase::ReadyToTeardown;
        return LeaveAction::Teardown;
    }
    if (++waited_ >= kReleaseTicks)
    {
        phase_ = Phase::Stuck;
        return LeaveAction::ReportStuck;
    }
    return LeaveAction::None;
}

}  // namespace lss::player_two
