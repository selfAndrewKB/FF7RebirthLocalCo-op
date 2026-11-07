// Host-side tests for players::SplitScreenSettings, run without the game.
//
// The console is a fake holding the three settings by name and the order of its writes. The cases: a join holds the
// three in SettingsOrder (the timing method before the gap recorder goes off) and logs each; a leave puts them back
// in the reverse order (the gap recorder back before the timing method); nothing is written meanwhile.

#include "players/split_screen_settings.h"

#include "expect.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using lss::players::kSplitScreenSettings;
using lss::players::SettingAccess;
using lss::players::SplitScreenSettings;

struct FakeConsole
{
    std::map<std::wstring, std::int32_t> values;
    std::vector<std::wstring>            writes;  // the names, in order
} g_console;

std::vector<std::wstring> g_lines;

std::optional<std::int32_t> Read(const wchar_t* name) noexcept
{
    const auto found = g_console.values.find(name);
    return found == g_console.values.end() ? std::nullopt : std::optional<std::int32_t>(found->second);
}

bool Write(const wchar_t* name, std::int32_t value) noexcept
{
    g_console.writes.emplace_back(name);
    g_console.values[name] = value;
    return true;
}

void Log(const std::wstring& line)
{
    g_lines.push_back(line);
}

SplitScreenSettings Fresh()
{
    g_console = {};
    g_lines.clear();
    for (const auto& setting : kSplitScreenSettings)
    {
        g_console.values[setting.name] = 7;  // the game's own value, never the mod's
    }
    return SplitScreenSettings(SettingAccess<std::int32_t>{&Read, &Write});
}

void TestJoinHoldsInOrder()
{
    std::puts("split screen settings: a join holds the three in order, each logged once");
    auto settings = Fresh();
    settings.Update(true, &Log);
    EXPECT(g_console.writes.size() == 3);
    EXPECT(g_console.writes[0] == L"r.SceneRenderTargetResizeMethod");
    EXPECT(g_console.writes[1] == L"r.DynamicRes.GPUTimingMeasureMethod");
    EXPECT(g_console.writes[2] == L"D3D12.EnableGapRecorder");
    EXPECT(g_console.values[L"D3D12.EnableGapRecorder"] == 0);
    EXPECT(g_lines.size() == 3);
    EXPECT(g_lines[0].find(L"split screen: r.SceneRenderTargetResizeMethod 7 -> 2") == 0);
    settings.Update(true, &Log);  // nothing more while a player stays in
    EXPECT(g_console.writes.size() == 3);
    EXPECT(g_lines.size() == 3);
}

void TestLeaveRestoresInReverse()
{
    std::puts("split screen settings: a leave puts them back, the gap recorder before the timing method");
    auto settings = Fresh();
    settings.Update(true, &Log);
    g_console.writes.clear();
    g_lines.clear();
    settings.Update(false, &Log);
    EXPECT(g_console.writes.size() == 3);
    EXPECT(g_console.writes[0] == L"r.SceneRenderTargetResizeMethod");
    EXPECT(g_console.writes[1] == L"D3D12.EnableGapRecorder");
    EXPECT(g_console.writes[2] == L"r.DynamicRes.GPUTimingMeasureMethod");
    for (const auto& setting : kSplitScreenSettings)
    {
        EXPECT(g_console.values[setting.name] == 7);
    }
    EXPECT(g_lines.size() == 3);
    EXPECT(g_lines[1].find(L"split screen: D3D12.EnableGapRecorder restored to 7") == 0);
    settings.Update(false, &Log);  // nothing more while no other player comes in
    EXPECT(g_console.writes.size() == 3);
}

}  // namespace

int main()
{
    TestJoinHoldsInOrder();
    TestLeaveRestoresInReverse();

    return lss::test::Report();
}
