#pragma once

// Player 2 is out and wants back in: when the join is tried, as a pure state machine (tested on the host:
// tests/outside_join_test.cpp). The session keeps the engine reads and the join itself (session.cpp OutsideTick).
//
//   Rejoin  a map load took player 2 out: once the new map has looked ready for kSettleTicks, a join is tried, up
//           to kTries times, within kRejoinWait of the load. The wait is timed, not counted in ticks: a load runs
//           uncapped (a few hundred ticks a second), so a tick count would give a long load less time than a short
//           one.
//   Await   player 2 was asked for with no party member free: no time limit. Once the field has looked ready for
//           kSettleTicks, the caller's member test runs every kAwaitCheckTicks (the member test is a few roster
//           reads, and a story party change may pass neither funnel, so this is much quicker than the in-game
//           backstop), or every tick while a look window is open (armed by the game's party changes; the window is
//           the session's, shared with its in-game retake, and counts down here only while the field is ready).
//           kTries joins that do not add player 2 end the wait.
//
// A wait that ends without player 2 reports why, for the one log line the session writes.

#include <chrono>
#include <cstdint>

namespace lss::session::outside_join {

inline constexpr std::uint32_t        kSettleTicks     = 180;
inline constexpr std::uint32_t        kTries           = 5;
inline constexpr std::chrono::seconds kRejoinWait{120};
inline constexpr std::uint32_t        kAwaitCheckTicks = 30;

enum class Mode
{
    None,
    Rejoin,
    Await,
};

enum class Action
{
    None,
    TryJoin,  // the schedule allows a join this tick (Await: if the caller's member test passes)
    GiveUp,   // the wait is over without player 2; `reason` says why
};

enum class Reason
{
    None,
    RejoinTimedOut,        // the field did not become ready within kRejoinWait
    RejoinRefused,         // an earlier join raised an exception
    RejoinTriesExhausted,  // kTries joins did not add player 2
    AwaitRefused,
    AwaitTriesExhausted,
};

struct Step
{
    Action        action  = Action::None;
    Reason        reason  = Reason::None;
    std::uint32_t attempt = 0;      // Rejoin's TryJoin: the try number, from 1
    bool          looking = false;  // Await's TryJoin: inside a look window (else the slow check)
};

class OutsideJoin
{
public:
    using Clock = std::chrono::steady_clock;

    Mode Current() const noexcept { return mode_; }

    /// A map load took player 2 out: join again once the new map is ready.
    void BeginRejoin(Clock::time_point now) noexcept;

    /// Player 2 waits for a free party member. Replaces a rejoin (the wait has no time limit). `fieldReady`: the
    /// player left a running field for this wait (the one-monitor wait outside), so the settle is done already and the
    /// first member test runs on the next ready tick.
    void BeginAwait(bool fieldReady = false) noexcept;

    /// Player 2 joined, or a leave was asked for: no wait.
    void End() noexcept;

    /// Once per tick while player 2 is out. `ready`: the field runs and the party is built (a join before that would
    /// be undone). `looks`: the look window's ticks left, counted down here while an Await is ready.
    Step Tick(Clock::time_point now, bool refuseJoins, bool ready, std::uint32_t& looks) noexcept;

    /// After a TryJoin's join: Rejoin ends once player 2 is in; Await counts a join that did not add player 2 and gives
    /// up at kTries. `wasMode` is the mode the TryJoin came from: a join that changed the wait (a rejoin's join found no
    /// free member and started an Await; a failed join asked for a leave) counts nothing.
    Step AfterJoin(Mode wasMode, bool hasPlayerTwo) noexcept;

private:
    Mode              mode_   = Mode::None;
    std::uint32_t     settle_ = 0;  // ticks the field has looked ready
    std::uint32_t     tries_  = 0;  // Rejoin: joins tried; Await: joins that did not add player 2
    std::uint32_t     slow_   = 0;  // Await: ticks of the slow check
    Clock::time_point asked_{};     // Rejoin: when the map load asked for it
};

}  // namespace lss::session::outside_join
