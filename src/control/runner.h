#pragma once

// Runs one parsed control script, a few commands per tick, against the mod (control::Target).
//
// Commands run in order, as many per tick as the script allows (up to kMaxCommandsPerTick):
// `wait` and `wait-until` are the only commands that hold the script back. A `stick` hold does not; it runs alongside the commands
// after it until its ticks run out, so both players' sticks, or a stick and a wait, can overlap.
// After its last command, a script lasts until its holds run out. The first command that fails
// stops the script at once and releases every hold still running.
//
// Pure logic with no engine access, tested on the host (tests/control_runner_test.cpp).

#include "control/script.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::control {

/// What a command did. `detail` says why when it failed, and may add a fact when it succeeded.
struct Outcome
{
    bool         ok = true;
    std::wstring detail;
};

/// The mod as a script sees it. Game thread only.
class Target
{
public:
    virtual ~Target() = default;

    virtual bool HasPlayerTwo() const = 0;
    virtual bool PlayerTwoPlaying() const = 0;  // player 2 plays their member

    virtual Outcome Join()                                     = 0;
    virtual Outcome Leave()                                    = 0;
    virtual Outcome SetOwnCamera(bool on)                      = 0;
    virtual Outcome SetPerPadSticks(bool on)                   = 0;
    virtual Outcome SetLayout(bool sideBySide)                 = 0;
    virtual Outcome SetBars(bool on)                           = 0;
    virtual Outcome SetSplitFov(float blend)                   = 0;  // negative: native
    virtual Outcome SetHudBand(bool on)                        = 0;
    virtual Outcome SetHudProbe(bool on)                       = 0;
    virtual Outcome HoldSticks(std::int32_t player, const float (&axes)[4]) = 0;
    virtual void    ReleaseSticks(std::int32_t player)         = 0;
    virtual Outcome Console(const std::wstring& command)       = 0;
    virtual Outcome Quit()                                     = 0;
    virtual Outcome Status()                                   = 0;
    virtual Outcome Note(const std::wstring& text)             = 0;
};

struct ScriptLine
{
    std::uint32_t number = 0;  // 1-based, in the script file
    Command       command;
};

/// One command's result, reported once when the command completes (a wait when it ends).
struct LineResult
{
    std::uint32_t line = 0;
    std::uint64_t tick = 0;  // the channel's tick count when the command completed
    std::wstring  command;   // as Describe writes it
    Outcome       outcome;
};

class Runner
{
public:
    /// A script without waits still spreads over ticks, so it never stalls one frame for long.
    static constexpr std::size_t kMaxCommandsPerTick = 32;

    explicit Runner(std::vector<ScriptLine> lines);

    /// Advances the script by one tick. Returns the commands that completed during it.
    std::vector<LineResult> Step(Target& target, std::uint64_t tick);

    /// Stops the script now (the channel is going away), releasing every hold.
    void Abort(Target& target);

    bool Finished() const noexcept { return finished_; }
    bool Failed() const noexcept { return failed_; }

private:
    static constexpr std::size_t kPlayers = 2;

    /// Runs the command at next_; returns false if the script must stop for this tick.
    bool RunNext(Target& target, std::uint64_t tick, std::vector<LineResult>& done);
    void CountDownHolds(Target& target);
    void ReleaseAll(Target& target);
    void Finish(Target& target, bool failed);

    std::vector<ScriptLine>                  lines_;
    std::size_t                              next_       = 0;
    std::uint32_t                            waitTicks_  = 0;   // ticks left in the current wait
    bool                                     waiting_    = false;
    std::array<std::uint32_t, kPlayers>      holdTicks_  = {};  // per player; 0 = no hold
    bool                                     finished_   = false;
    bool                                     failed_     = false;
};

}  // namespace lss::control
