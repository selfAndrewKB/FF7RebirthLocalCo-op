#include "player_two/session/outside_join.h"

namespace lss::player_two::outside_join {

void OutsideJoin::BeginRejoin(Clock::time_point now) noexcept
{
    mode_   = Mode::Rejoin;
    settle_ = 0;
    tries_  = 0;
    asked_  = now;
}

void OutsideJoin::BeginAwait() noexcept
{
    mode_   = Mode::Await;
    settle_ = 0;
    tries_  = 0;
    slow_   = 0;
}

void OutsideJoin::End() noexcept
{
    mode_ = Mode::None;
}

Step OutsideJoin::Tick(Clock::time_point now, bool refuseJoins, bool ready, std::uint32_t& looks) noexcept
{
    Step step;
    switch (mode_)
    {
        case Mode::None: break;
        case Mode::Rejoin:
            if (now - asked_ > kRejoinWait)
            {
                mode_       = Mode::None;
                step.action = Action::GiveUp;
                step.reason = Reason::RejoinTimedOut;
                break;
            }
            if (refuseJoins)
            {
                mode_       = Mode::None;  // the join would refuse and say why
                step.action = Action::GiveUp;
                step.reason = Reason::RejoinRefused;
                break;
            }
            if (!ready)
            {
                settle_ = 0;
                break;
            }
            if (++settle_ < kSettleTicks)
            {
                break;
            }
            settle_ = 0;
            if (++tries_ > kTries)
            {
                mode_       = Mode::None;
                step.action = Action::GiveUp;
                step.reason = Reason::RejoinTriesExhausted;
                break;
            }
            step.action  = Action::TryJoin;
            step.attempt = tries_;
            break;
        case Mode::Await:
            if (refuseJoins)
            {
                mode_       = Mode::None;
                step.action = Action::GiveUp;
                step.reason = Reason::AwaitRefused;
                break;
            }
            if (!ready)
            {
                settle_ = 0;
                break;
            }
            if (settle_ < kSettleTicks)
            {
                ++settle_;
                break;
            }
            step.looking = looks > 0;
            if (step.looking)
            {
                --looks;
            }
            else if (++slow_ % kAwaitCheckTicks != 0)
            {
                break;
            }
            step.action = Action::TryJoin;
            break;
    }
    return step;
}

Step OutsideJoin::AfterJoin(Mode wasMode, bool hasPlayerTwo) noexcept
{
    Step step;
    if (mode_ != wasMode)
    {
        return step;  // the join changed the wait
    }
    if (hasPlayerTwo)
    {
        mode_ = Mode::None;
        return step;
    }
    if (mode_ == Mode::Await && ++tries_ >= kTries)
    {
        mode_       = Mode::None;
        step.action = Action::GiveUp;
        step.reason = Reason::AwaitTriesExhausted;
    }
    return step;
}

}  // namespace lss::player_two::outside_join
