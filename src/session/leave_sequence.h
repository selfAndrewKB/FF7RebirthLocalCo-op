#pragma once

// The order in which player 2 leaves, as a small state machine (tested on the host:
// tests/leave_sequence_test.cpp).
//
// The order is forced by two unguarded engine writes:
//   1. Release the party member first, while player 2's trigger actor still exists: the switch
//      writes through that trigger unconditionally, so releasing after destroying it faults.
//      The release completes over later ticks, so the sequence waits until the member is back
//      with its AI.
//   2. Then, in one game-thread step, destroy player 2's four leftover actors and remove player 2:
//      player 2's camera update reads its camera actor without a null check, so no camera update
//      may run between destroying that actor and destroying the camera manager.
//
// A map load cannot wait for the release: the sequence then releases and tears down at once.

#include <cstdint>

namespace lss::session {

enum class LeaveAction
{
    None,        // nothing to do this tick
    Release,     // hand the member back to its AI
    Teardown,    // destroy the leftover actors and remove player 2, in this same step
    ReportStuck  // the release did not complete in time; player 2 stays in
};

class LeaveSequence
{
public:
    /// Ticks to wait for a release before giving up (about two seconds at 60 fps).
    static constexpr std::uint32_t kReleaseTicks = 120;

    /// Starts a leave. `holdsMember` says whether player 2 currently holds a party member.
    /// Returns the first action. A leave already under way is not restarted.
    LeaveAction Begin(bool holdsMember) noexcept;

    /// Once per tick while a leave is under way. `released` is whether the member is back with its
    /// AI (ignored when there is none).
    LeaveAction Next(bool released) noexcept;

    bool InProgress() const noexcept { return phase_ != Phase::Idle; }

    /// After Teardown was carried out (or the leave was abandoned).
    void Finish() noexcept { phase_ = Phase::Idle; }

private:
    enum class Phase
    {
        Idle,
        AwaitingRelease,
        ReadyToTeardown,
        Stuck,
    };

    Phase         phase_ = Phase::Idle;
    std::uint32_t waited_ = 0;
};

}  // namespace lss::session
