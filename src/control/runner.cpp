#include "control/runner.h"

#include <format>
#include <utility>

namespace lss::control {

namespace {

bool ConditionHolds(const Target& target, WaitCondition condition)
{
    return condition == WaitCondition::Playing ? target.PlayerTwoPlaying() : !target.HasPlayerTwo();
}

Outcome Run(Target& target, const Command& command)
{
    switch (command.kind)
    {
        case CommandKind::Join: return target.Join();
        case CommandKind::Leave: return target.Leave();
        case CommandKind::OwnCamera: return target.SetOwnCamera(command.on);
        case CommandKind::Sticks: return target.SetPerPadSticks(command.on);
        case CommandKind::Layout: return target.SetLayout(command.on);
        case CommandKind::Bars: return target.SetBars(command.on);
        case CommandKind::SplitFov: return target.SetSplitFov(command.blend);
        case CommandKind::HudBand: return target.SetHudBand(command.on);
        case CommandKind::Stick: return target.HoldSticks(command.player, command.axes);
        case CommandKind::Console: return target.Console(command.text);
        case CommandKind::Quit: return target.Quit();
        case CommandKind::Status: return target.Status();
        case CommandKind::Note: return target.Note(command.text);
        case CommandKind::Wait:
        case CommandKind::WaitUntil: break;  // handled by the runner itself
    }
    return {false, L"not a command the target runs"};
}

}  // namespace

Runner::Runner(std::vector<ScriptLine> lines) : lines_(std::move(lines)) {}

std::vector<LineResult> Runner::Step(Target& target, std::uint64_t tick)
{
    std::vector<LineResult> done;
    if (finished_)
    {
        return done;
    }
    CountDownHolds(target);

    if (waiting_)
    {
        const ScriptLine& line = lines_[next_];
        if (line.command.kind == CommandKind::WaitUntil)
        {
            if (!ConditionHolds(target, line.command.condition))
            {
                if (--waitTicks_ > 0)
                {
                    return done;
                }
                done.push_back({line.number, tick, Describe(line.command),
                                {false, std::format(L"timed out after {} ticks", line.command.ticks)}});
                Finish(target, true);
                return done;
            }
        }
        else if (--waitTicks_ > 0)
        {
            return done;
        }
        waiting_ = false;
        done.push_back({line.number, tick, Describe(line.command), {}});
        ++next_;
    }

    for (std::size_t ran = 0; next_ < lines_.size(); ++ran)
    {
        if (ran == kMaxCommandsPerTick || !RunNext(target, tick, done))
        {
            return done;
        }
    }
    bool holding = false;
    for (const auto ticks : holdTicks_)
    {
        holding = holding || ticks > 0;
    }
    if (!holding)
    {
        Finish(target, false);
    }
    return done;
}

bool Runner::RunNext(Target& target, std::uint64_t tick, std::vector<LineResult>& done)
{
    const ScriptLine& line    = lines_[next_];
    const Command&    command = line.command;
    if (command.kind == CommandKind::Wait
        || (command.kind == CommandKind::WaitUntil && !ConditionHolds(target, command.condition)))
    {
        waiting_   = true;
        waitTicks_ = command.ticks;
        return false;
    }
    if (command.kind == CommandKind::WaitUntil)
    {
        done.push_back({line.number, tick, Describe(command), {}});
        ++next_;
        return true;
    }

    Outcome outcome = Run(target, command);
    const bool ok   = outcome.ok;
    done.push_back({line.number, tick, Describe(command), std::move(outcome)});
    if (!ok)
    {
        Finish(target, true);
        return false;
    }
    if (command.kind == CommandKind::Stick)
    {
        holdTicks_[static_cast<std::size_t>(command.player - 1)] = command.ticks;
    }
    ++next_;
    return true;
}

void Runner::CountDownHolds(Target& target)
{
    for (std::size_t i = 0; i < kPlayers; ++i)
    {
        if (holdTicks_[i] > 0 && --holdTicks_[i] == 0)
        {
            target.ReleaseSticks(static_cast<std::int32_t>(i + 1));
        }
    }
}

void Runner::ReleaseAll(Target& target)
{
    for (std::size_t i = 0; i < kPlayers; ++i)
    {
        if (holdTicks_[i] > 0)
        {
            holdTicks_[i] = 0;
            target.ReleaseSticks(static_cast<std::int32_t>(i + 1));
        }
    }
}

void Runner::Finish(Target& target, bool failed)
{
    ReleaseAll(target);
    finished_ = true;
    failed_   = failed;
}

void Runner::Abort(Target& target)
{
    if (!finished_)
    {
        Finish(target, true);
    }
}

}  // namespace lss::control
