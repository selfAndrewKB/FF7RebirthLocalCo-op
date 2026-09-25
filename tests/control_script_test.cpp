// Host-side tests for control::ParseLine, run without the game.

#include "control/script.h"

#include <cstdio>
#include <initializer_list>

namespace {

using lss::control::CommandKind;
using lss::control::ParseLine;
using lss::control::WaitCondition;

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

bool Rejected(const wchar_t* line)
{
    const auto parsed = ParseLine(line);
    return !parsed.command && !parsed.error.empty();
}

void TestSkippedLines()
{
    std::puts("script: blank lines and comments are skipped without an error");
    for (const wchar_t* line : {L"", L"   ", L"# a comment", L"  # indented comment", L"\r"})
    {
        const auto parsed = ParseLine(line);
        EXPECT(!parsed.command && parsed.error.empty());
    }
}

void TestSimpleCommands()
{
    std::puts("script: commands without arguments, surrounding space and a CR ignored");
    EXPECT(ParseLine(L"join").command->kind == CommandKind::Join);
    EXPECT(ParseLine(L"  leave  ").command->kind == CommandKind::Leave);
    EXPECT(ParseLine(L"quit\r").command->kind == CommandKind::Quit);
    EXPECT(ParseLine(L"status").command->kind == CommandKind::Status);
    EXPECT(Rejected(L"join now"));
    EXPECT(Rejected(L"JOIN"));
    EXPECT(Rejected(L"dance"));
}

void TestSwitches()
{
    std::puts("script: own-camera and sticks take exactly one of their two words");
    const auto own = ParseLine(L"own-camera on");
    EXPECT(own.command && own.command->kind == CommandKind::OwnCamera && own.command->on);
    EXPECT(!ParseLine(L"own-camera off").command->on);
    EXPECT(ParseLine(L"sticks per-pad").command->on);
    EXPECT(!ParseLine(L"sticks merged").command->on);
    EXPECT(Rejected(L"own-camera"));
    EXPECT(Rejected(L"own-camera yes"));
    EXPECT(Rejected(L"sticks per-pad extra"));
    EXPECT(ParseLine(L"sticks-mask 4").command->ticks == 4);
    EXPECT(Rejected(L"sticks-mask 8"));
    EXPECT(Rejected(L"leak-fixes 8"));
    EXPECT(Rejected(L"sticks-mask -1"));
    EXPECT(Rejected(L"sticks-mask"));
    EXPECT(Rejected(L"hud-levers-mask 64"));
}

void TestLayoutAndBars()
{
    std::puts("script: layout top-bottom|side-by-side and bars on|off");
    const auto side = ParseLine(L"layout side-by-side");
    EXPECT(side.command && side.command->kind == CommandKind::Layout && side.command->on);
    const auto top = ParseLine(L"layout top-bottom");
    EXPECT(top.command && top.command->kind == CommandKind::Layout && !top.command->on);
    const auto bars = ParseLine(L"bars on");
    EXPECT(bars.command && bars.command->kind == CommandKind::Bars && bars.command->on);
    EXPECT(!ParseLine(L"bars off").command->on);
    EXPECT(Rejected(L"layout"));
    EXPECT(Rejected(L"layout vertical"));
    EXPECT(Rejected(L"bars"));
    EXPECT(Rejected(L"bars yes"));
}

void TestSplitFov()
{
    std::puts("script: split-fov <blend 0..1>|native");
    const auto half = ParseLine(L"split-fov 0.5");
    EXPECT(half.command && half.command->kind == CommandKind::SplitFov && half.command->blend == 0.5f);
    const auto native = ParseLine(L"split-fov native");
    EXPECT(native.command && native.command->kind == CommandKind::SplitFov && native.command->blend < 0.0f);
    EXPECT(ParseLine(L"split-fov 0").command->blend == 0.0f);
    EXPECT(ParseLine(L"split-fov 1").command->blend == 1.0f);
    EXPECT(Rejected(L"split-fov"));
    EXPECT(Rejected(L"split-fov -0.5"));
    EXPECT(Rejected(L"split-fov 1.5"));
    EXPECT(Rejected(L"split-fov wide"));
}

void TestStick()
{
    std::puts("script: stick <player> <lx> <ly> <rx> <ry> <ticks>, axes within -1..1");
    const auto parsed = ParseLine(L"stick 2 0 1 -0.5 0.25 120");
    EXPECT(parsed.command && parsed.command->kind == CommandKind::Stick);
    if (parsed.command)
    {
        EXPECT(parsed.command->player == 2);
        EXPECT(parsed.command->axes[0] == 0.0f && parsed.command->axes[1] == 1.0f);
        EXPECT(parsed.command->axes[2] == -0.5f && parsed.command->axes[3] == 0.25f);
        EXPECT(parsed.command->ticks == 120);
    }
    EXPECT(Rejected(L"stick 3 0 1 0 0 10"));    // no player 3
    EXPECT(Rejected(L"stick 1 0 1.5 0 0 10"));  // axis out of range
    EXPECT(Rejected(L"stick 1 0 1 0 0 0"));     // zero ticks
    EXPECT(Rejected(L"stick 1 0 1 0 0"));       // missing ticks
    EXPECT(Rejected(L"stick 1 0 x 0 0 10"));    // not a number
    EXPECT(Rejected(L"stick 1 0 1 0 0 36001")); // over the limit
}

void TestWaits()
{
    std::puts("script: wait <ticks>; wait-until playing|out [<ticks>] with a default timeout");
    EXPECT(ParseLine(L"wait 30").command->ticks == 30);
    EXPECT(Rejected(L"wait"));
    EXPECT(Rejected(L"wait -5"));
    EXPECT(Rejected(L"wait 0"));
    const auto until = ParseLine(L"wait-until out");
    EXPECT(until.command && until.command->condition == WaitCondition::Out);
    EXPECT(until.command && until.command->ticks == lss::control::kDefaultWaitUntilTicks);
    const auto timed = ParseLine(L"wait-until playing 900");
    EXPECT(timed.command && timed.command->condition == WaitCondition::Playing && timed.command->ticks == 900);
    EXPECT(Rejected(L"wait-until dancing"));
}

void TestTextCommands()
{
    std::puts("script: console and note keep the rest of the line, and need some");
    const auto console = ParseLine(L"console r.AllowOcclusionQueries 0");
    EXPECT(console.command && console.command->kind == CommandKind::Console);
    EXPECT(console.command && console.command->text == L"r.AllowOcclusionQueries 0");
    const auto note = ParseLine(L"note  walking north  ");
    EXPECT(note.command && note.command->text == L"walking north");
    EXPECT(Rejected(L"console"));
    EXPECT(Rejected(L"note   "));
}

void TestDescribeRoundTrips()
{
    std::puts("script: Describe writes a line that parses back to the same command");
    for (const wchar_t* line : {L"join", L"own-camera off", L"sticks per-pad", L"layout side-by-side",
                                L"layout top-bottom", L"bars on", L"bars off", L"split-fov 0.5",
                                L"split-fov native", L"hud-band on", L"hud-band off", L"hud-probe on", L"hud-probe off", L"dump widgets", L"dump slots Command_C", L"dump hud-players", L"hud-trace 5", L"hud-image on", L"hud-image off", L"hud-levers on", L"hud-levers off", L"hud-levers-mask 0", L"hud-levers-mask 63", L"sticks-mask 0", L"sticks-mask 7", L"leak-fixes 0", L"leak-fixes 5", L"stick 1 0 1 0 0 60",
                                L"wait 5",
                                L"wait-until playing 600", L"console stat fps", L"note hello"})
    {
        const auto parsed = ParseLine(line);
        EXPECT(parsed.command && lss::control::Describe(*parsed.command) == line);
    }
}

}  // namespace

int main()
{
    TestSkippedLines();
    TestSimpleCommands();
    TestSwitches();
    TestLayoutAndBars();
    TestSplitFov();
    TestStick();
    TestWaits();
    TestTextCommands();
    TestDescribeRoundTrips();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
