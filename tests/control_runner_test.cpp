// Host-side tests for control::Runner, run without the game, against a fake mod.

#include "control/runner.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using lss::control::Command;
using lss::control::LineResult;
using lss::control::Outcome;
using lss::control::ParseLine;
using lss::control::Runner;
using lss::control::ScriptLine;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

/// Records every call; player 2's state is set by the test.
class FakeMod final : public lss::control::Target
{
public:
    bool                      playerTwo = false;
    bool                      playing   = false;
    bool                      perPad    = true;
    std::vector<std::wstring> calls;

    bool HasPlayerTwo() const override { return playerTwo; }
    bool PlayerTwoPlaying() const override { return playing; }

    Outcome Join() override { return Record(L"join"); }
    Outcome Leave() override { return Record(L"leave"); }
    Outcome SetOwnCamera(bool on) override { return Record(on ? L"own-camera on" : L"own-camera off"); }
    Outcome SetPerPadSticks(bool on) override { return Record(on ? L"sticks per-pad" : L"sticks merged"); }
    Outcome SetLayout(bool sideBySide) override { return Record(sideBySide ? L"side-by-side" : L"top-bottom"); }
    Outcome SetBars(bool on) override { return Record(on ? L"bars on" : L"bars off"); }
    Outcome SetSplitFov(float blend) override { return Record(L"split-fov " + std::to_wstring(blend)); }
    Outcome SetHudBand(bool on) override { return Record(on ? L"hud-band on" : L"hud-band off"); }
    Outcome HoldSticks(std::int32_t player, const float (&axes)[4]) override
    {
        if (!perPad)
        {
            return {false, L"per-pad sticks are off"};
        }
        return Record(L"hold " + std::to_wstring(player) + L" ly=" + std::to_wstring(static_cast<int>(axes[1])));
    }
    void    ReleaseSticks(std::int32_t player) override { Record(L"release " + std::to_wstring(player)); }
    Outcome Console(const std::wstring& command) override { return Record(L"console " + command); }
    Outcome Quit() override { return Record(L"quit"); }
    Outcome Status() override { return Record(L"status"); }
    Outcome Note(const std::wstring& text) override { return Record(L"note " + text); }

private:
    Outcome Record(std::wstring call)
    {
        calls.push_back(std::move(call));
        return {};
    }
};

Runner Script(std::initializer_list<const wchar_t*> source)
{
    std::vector<ScriptLine> lines;
    std::uint32_t           number = 0;
    for (const wchar_t* text : source)
    {
        ++number;
        if (auto parsed = ParseLine(text); parsed.command)
        {
            lines.push_back({number, *parsed.command});
        }
    }
    return Runner(std::move(lines));
}

void TestRunsInOrderInOneTick()
{
    std::puts("runner: commands with no wait all run in the first tick, in order");
    FakeMod mod;
    auto    runner = Script({L"join", L"note a", L"status"});
    const auto done = runner.Step(mod, 1);
    EXPECT(done.size() == 3);
    EXPECT((mod.calls == std::vector<std::wstring>{L"join", L"note a", L"status"}));
    EXPECT(runner.Finished() && !runner.Failed());
    EXPECT(done.size() == 3 && done[2].line == 3 && done[2].tick == 1 && done[2].command == L"status");
}

void TestWaitHoldsTheScriptBack()
{
    std::puts("runner: wait 3 at tick 1 lets the next command run at tick 4");
    FakeMod mod;
    auto    runner = Script({L"wait 3", L"note after"});
    runner.Step(mod, 1);
    runner.Step(mod, 2);
    runner.Step(mod, 3);
    EXPECT(mod.calls.empty());
    const auto done = runner.Step(mod, 4);
    EXPECT((mod.calls == std::vector<std::wstring>{L"note after"}));
    EXPECT(done.size() == 2 && done[0].command == L"wait 3" && done[0].tick == 4);
    EXPECT(runner.Finished());
}

void TestWaitUntilPassesOrTimesOut()
{
    std::puts("runner: wait-until continues once the condition holds, and fails after its timeout");
    FakeMod mod;
    auto    runner = Script({L"wait-until playing 10", L"note playing"});
    runner.Step(mod, 1);
    EXPECT(mod.calls.empty() && !runner.Finished());
    mod.playing = true;
    runner.Step(mod, 2);
    EXPECT((mod.calls == std::vector<std::wstring>{L"note playing"}));
    EXPECT(runner.Finished() && !runner.Failed());

    FakeMod never;
    auto    timeout = Script({L"wait-until out 2", L"note unreachable"});
    never.playerTwo = true;
    timeout.Step(never, 1);
    timeout.Step(never, 2);
    const auto done = timeout.Step(never, 3);
    EXPECT(timeout.Finished() && timeout.Failed());
    EXPECT(done.size() == 1 && !done[0].outcome.ok);
    EXPECT(never.calls.empty());

    FakeMod already;
    auto    immediate = Script({L"wait-until out", L"note out"});
    immediate.Step(already, 1);
    EXPECT(immediate.Finished() && (already.calls == std::vector<std::wstring>{L"note out"}));
}

void TestHoldsOverlapAndExpire()
{
    std::puts("runner: two holds run alongside a wait, each released after its own ticks");
    FakeMod mod;
    auto    runner = Script({L"stick 1 0 1 0 0 2", L"stick 2 0 -1 0 0 3", L"wait 5"});
    runner.Step(mod, 1);
    EXPECT((mod.calls == std::vector<std::wstring>{L"hold 1 ly=1", L"hold 2 ly=-1"}));
    runner.Step(mod, 2);
    EXPECT(mod.calls.size() == 2);
    runner.Step(mod, 3);
    EXPECT(mod.calls.size() == 3 && mod.calls[2] == L"release 1");
    runner.Step(mod, 4);
    EXPECT(mod.calls.size() == 4 && mod.calls[3] == L"release 2");
    runner.Step(mod, 5);
    runner.Step(mod, 6);
    EXPECT(runner.Finished() && mod.calls.size() == 4);
}

void TestFailureStopsAndReleases()
{
    std::puts("runner: a failed command stops the script and releases every hold");
    FakeMod mod;
    auto    runner = Script({L"stick 1 0 1 0 0 100", L"note before"});
    runner.Step(mod, 1);
    mod.perPad = false;
    auto failing = Script({L"stick 2 0 1 0 0 10", L"note never"});
    const auto done = failing.Step(mod, 1);
    EXPECT(failing.Finished() && failing.Failed());
    EXPECT(done.size() == 1 && !done[0].outcome.ok && done[0].outcome.detail == L"per-pad sticks are off");
    EXPECT(mod.calls.back() != L"note never");

    runner.Abort(mod);
    EXPECT(runner.Finished() && mod.calls.back() == L"release 1");
}

void TestScriptEndsWhenItsHoldsDo()
{
    std::puts("runner: after its last command, a script lasts until its holds run out");
    FakeMod mod;
    auto    runner = Script({L"stick 2 0 1 0 0 3"});
    runner.Step(mod, 1);
    runner.Step(mod, 2);
    runner.Step(mod, 3);
    EXPECT(!runner.Finished() && mod.calls.size() == 1);
    runner.Step(mod, 4);
    EXPECT(runner.Finished() && !runner.Failed());
    EXPECT((mod.calls == std::vector<std::wstring>{L"hold 2 ly=1", L"release 2"}));
}

void TestCommandsPerTickAreCapped()
{
    std::puts("runner: at most kMaxCommandsPerTick commands run in one tick; the rest follow on the next");
    FakeMod                  mod;
    std::vector<ScriptLine> lines;
    for (std::uint32_t i = 1; i <= Runner::kMaxCommandsPerTick + 5; ++i)
    {
        lines.push_back({i, *ParseLine(L"status").command});
    }
    Runner runner(std::move(lines));
    runner.Step(mod, 1);
    EXPECT(mod.calls.size() == Runner::kMaxCommandsPerTick && !runner.Finished());
    runner.Step(mod, 2);
    EXPECT(mod.calls.size() == Runner::kMaxCommandsPerTick + 5 && runner.Finished());
}

}  // namespace

int main()
{
    TestRunsInOrderInOneTick();
    TestWaitHoldsTheScriptBack();
    TestWaitUntilPassesOrTimesOut();
    TestHoldsOverlapAndExpire();
    TestFailureStopsAndReleases();
    TestScriptEndsWhenItsHoldsDo();
    TestCommandsPerTickAreCapped();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
