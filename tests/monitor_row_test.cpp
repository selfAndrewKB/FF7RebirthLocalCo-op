// Host-side tests for views::monitor_row::Choose, run without the game.
//
// The cases: the user's three-monitor row (TV set to 1440p at the left, two 1440p monitors to its right, an
// ultrawide above); a two-column span starting at the game's monitor and shifted left at the row's end; the row
// broken by a monitor of another size, a misaligned top, a gap, or Windows scaling; the game's monitor scaled;
// one column always possible; bad inputs.

#include "views/monitor_row.h"

#include <cstdio>
#include <vector>

namespace {

using lss::views::monitor_row::Choice;
using lss::views::monitor_row::Choose;
using lss::views::monitor_row::Monitor;
using lss::views::monitor_row::Rect;

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

Monitor At(std::int32_t left, std::int32_t top, std::int32_t width = 2560, std::int32_t height = 1440,
           std::uint32_t dpi = 96)
{
    return {{left, top, left + width, top + height}, dpi};
}

/// The user's monitors on 2026-10-20: the ultrawide primary above, the row of three below (the TV at 1440p).
std::vector<Monitor> UsersMonitors()
{
    return {At(0, 0, 3440, 1440), At(-1971, 1440), At(589, 1440), At(3149, 1440)};
}

void TestThreeAcrossTheRow()
{
    std::printf("three columns across the user's row, from any of its monitors\n");
    const auto monitors = UsersMonitors();
    for (std::size_t game = 1; game <= 3; ++game)
    {
        const Choice c = Choose(monitors, game, 3);
        EXPECT(c.ok);
        EXPECT(c.rowMonitors == 3);
        EXPECT((c.span == Rect{-1971, 1440, 5709, 2880}));
    }
}

void TestTwoColumnsStartAtTheGame()
{
    std::printf("two columns start at the game's monitor and extend right; shifted left at the row's end\n");
    const auto monitors = UsersMonitors();
    Choice     c        = Choose(monitors, 1, 2);
    EXPECT((c.ok && c.span == Rect{-1971, 1440, 3149, 2880}));
    c = Choose(monitors, 2, 2);
    EXPECT((c.ok && c.span == Rect{589, 1440, 5709, 2880}));
    c = Choose(monitors, 3, 2);  // the rightmost: no monitor to its right, so the span shifts left
    EXPECT((c.ok && c.span == Rect{589, 1440, 5709, 2880}));
}

void TestOneColumnIsTheGamesMonitor()
{
    std::printf("one column is the game's own monitor, even an ultrawide alone\n");
    const auto   monitors = UsersMonitors();
    const Choice c        = Choose(monitors, 0, 1);
    EXPECT((c.ok && c.span == Rect{0, 0, 3440, 1440} && c.rowMonitors == 1));
}

void TestRowBrokenByAnotherSize()
{
    std::printf("the TV at 4K breaks the row: two equal monitors, the reason names the TV\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(-3251, 1440, 3840, 2160), At(589, 1440), At(3149, 1440)};
    const Choice         c = Choose(monitors, 2, 3);
    EXPECT(!c.ok);
    EXPECT(c.rowMonitors == 2);
    EXPECT(c.why.find(L"2 equal monitor(s)") != std::wstring::npos);
    EXPECT(c.why.find(L"3840x2160") != std::wstring::npos);
}

void TestRowBrokenByAMisalignedTop()
{
    std::printf("a monitor 13 px lower is not in the row\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(589, 1453), At(3149, 1453)};
    const Choice         c = Choose(monitors, 1, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"top edge") != std::wstring::npos);
}

void TestRowBrokenByAGap()
{
    std::printf("a gap between monitors ends the row without a named monitor\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(600, 1440), At(3160, 1440)};
    const Choice         c = Choose(monitors, 0, 2);
    EXPECT(!c.ok && c.rowMonitors == 1);
    EXPECT(c.why.find(L"1 equal monitor(s)") != std::wstring::npos);
}

void TestScalingBreaksTheRow()
{
    std::printf("a scaled neighbour is excluded; a scaled game monitor refuses outright\n");
    std::vector<Monitor> monitors{At(-1971, 1440, 2560, 1440, 168), At(589, 1440), At(3149, 1440)};
    Choice               c = Choose(monitors, 1, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"175 %") != std::wstring::npos);
    c = Choose(monitors, 0, 1);
    EXPECT(!c.ok && c.why.find(L"the game's monitor is scaled") != std::wstring::npos);
}

void TestBadInputs()
{
    std::printf("bad inputs refuse\n");
    const auto monitors = UsersMonitors();
    EXPECT(!Choose(monitors, 9, 2).ok);
    EXPECT(!Choose(monitors, 1, 0).ok);
    EXPECT(!Choose({}, 0, 1).ok);
}

}  // namespace

int main()
{
    TestThreeAcrossTheRow();
    TestTwoColumnsStartAtTheGame();
    TestOneColumnIsTheGamesMonitor();
    TestRowBrokenByAnotherSize();
    TestRowBrokenByAMisalignedTop();
    TestRowBrokenByAGap();
    TestScalingBreaksTheRow();
    TestBadInputs();
    if (g_failures == 0)
    {
        std::printf("all monitor_row tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
