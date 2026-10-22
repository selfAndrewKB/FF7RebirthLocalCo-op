// Host-side tests for players::HeldSetting, run without the game.
//
// The setting is a fake holding one integer by name. The cases are the join/leave transitions a held setting must
// follow: set once on the hold, put back on the release, nothing touched meanwhile, a value someone else chose left
// alone, failures reported rather than assumed, and the mod's own later writes still going back to the first
// value. Then the split-screen console settings' order, the three-player layout's hold, and every change's text.

#include "players/held_setting.h"
#include "players/split_screen_settings.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using lss::players::HeldChange;
using lss::players::HeldSetting;
using lss::players::SettingAccess;
using Kind = HeldChange::Kind;

constexpr const wchar_t* kName = L"r.SceneRenderTargetResizeMethod";
constexpr std::int32_t   kHeld = 2;

// The fake setting: one value, and switches to make reading or writing fail.
struct FakeSetting
{
    std::int32_t value        = 0;
    bool         readWorks    = true;
    bool         writeWorks   = true;
    bool         writeIgnored = false;  // the write "runs" but changes nothing
    int          writes       = 0;
    std::wstring name         = kName;  // the one setting it holds
} g_setting;

std::optional<std::int32_t> Read(const wchar_t* name) noexcept
{
    if (!g_setting.readWorks || name != g_setting.name)
    {
        return std::nullopt;
    }
    return g_setting.value;
}

bool Write(const wchar_t* name, std::int32_t value) noexcept
{
    ++g_setting.writes;
    if (!g_setting.writeWorks || name != g_setting.name)
    {
        return false;
    }
    if (!g_setting.writeIgnored)
    {
        g_setting.value = value;
    }
    return true;
}

HeldSetting<std::int32_t> Fresh(std::int32_t initial)
{
    g_setting       = FakeSetting{};
    g_setting.value = initial;
    return HeldSetting<std::int32_t>(SettingAccess<std::int32_t>{&Read, &Write}, kName);
}

void TestJoinSetsAndLeaveRestores()
{
    std::puts("join sets 2 once; leave puts the previous value back");
    auto setting = Fresh(0);
    EXPECT(setting.Update(false, kHeld).kind == Kind::None);  // one player: nothing touched
    EXPECT(g_setting.writes == 0);

    const auto joined = setting.Update(true, kHeld);
    EXPECT(joined.kind == Kind::Applied && joined.value == 0 && joined.held == kHeld);
    EXPECT(g_setting.value == 2);
    EXPECT(setting.In());
    EXPECT(setting.Update(true, kHeld).kind == Kind::None);  // only on a change
    EXPECT(g_setting.writes == 1);

    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::Restored && left.value == 0 && left.held == kHeld);
    EXPECT(g_setting.value == 0);
    EXPECT(!setting.In());
}

void TestAlreadySetIsNotRestored()
{
    std::puts("a value that was already 2 is neither set nor restored");
    auto setting = Fresh(2);
    const auto joined = setting.Update(true, kHeld);
    EXPECT(joined.kind == Kind::AlreadySet && joined.value == 2 && joined.held == 2);
    EXPECT(setting.Update(false, kHeld).kind == Kind::None);
    EXPECT(g_setting.value == 2);
    EXPECT(g_setting.writes == 0);
}

void TestSomeoneElsesChoiceIsKept()
{
    std::puts("a value changed while player 2 was in is left alone on leave");
    auto setting = Fresh(0);
    setting.Update(true, kHeld);
    g_setting.value = 1;  // e.g. typed into the console
    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::LeftAlone && left.value == 1 && left.held == kHeld);
    EXPECT(g_setting.value == 1);
}

void TestFailuresAreReported()
{
    std::puts("unreadable, unwritable or ignored writes are reported, not assumed");
    {
        auto setting = Fresh(0);
        g_setting.readWorks = false;
        EXPECT(setting.Update(true, kHeld).kind == Kind::ApplyFailed);
        g_setting.readWorks = true;
        EXPECT(setting.Update(false, kHeld).kind == Kind::None);  // nothing of ours to undo
        EXPECT(g_setting.value == 0);
    }
    {
        auto setting = Fresh(0);
        g_setting.writeWorks = false;
        const auto joined = setting.Update(true, kHeld);
        EXPECT(joined.kind == Kind::ApplyFailed && joined.value == 0 && joined.held == kHeld);
    }
    {
        auto setting = Fresh(0);
        g_setting.writeIgnored = true;  // read-back shows the write did not take
        EXPECT(setting.Update(true, kHeld).kind == Kind::ApplyFailed);
        EXPECT(setting.Update(false, kHeld).kind == Kind::None);
    }
    {
        auto setting = Fresh(0);
        setting.Update(true, kHeld);
        g_setting.writeWorks = false;
        const auto left = setting.Update(false, kHeld);
        EXPECT(left.kind == Kind::RestoreFailed && left.value == 0);
    }
}

void TestFailedRestoreIsRetriedAtTheNextLeave()
{
    std::puts("a failed restore keeps the old value and retries at the next leave");
    auto setting = Fresh(0);
    setting.Update(true, kHeld);
    g_setting.writeWorks = false;
    EXPECT(setting.Update(false, kHeld).kind == Kind::RestoreFailed);
    EXPECT(g_setting.value == 2);

    g_setting.writeWorks = true;
    EXPECT(setting.Update(true, kHeld).kind == Kind::AlreadySet);  // still 2 from before
    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::Restored && left.value == 0);  // the original, not 2
    EXPECT(g_setting.value == 0);
}

void TestChoiceAfterFailedRestoreIsWhatComesBack()
{
    std::puts("a value someone chose after a failed restore is the one put back");
    auto setting = Fresh(0);
    setting.Update(true, kHeld);  // 0 -> 2
    g_setting.readWorks = false;
    EXPECT(setting.Update(false, kHeld).kind == Kind::RestoreFailed);  // 0 still owed
    g_setting.readWorks = true;
    g_setting.value     = 1;                                          // their choice, with player 2 out
    EXPECT(setting.Update(true, kHeld).kind == Kind::Applied);         // 1 -> 2: 1 is not the mod's write
    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::Restored && left.value == 1);
    EXPECT(g_setting.value == 1);
}

// A setting where exactly one read, counted from 0 after Fresh, fails.
int g_reads    = 0;
int g_failRead = -1;

std::optional<std::int32_t> FlakyRead(const wchar_t* name) noexcept
{
    return g_reads++ == g_failRead ? std::nullopt : Read(name);
}

HeldSetting<std::int32_t> FreshFlaky(std::int32_t initial, int failRead)
{
    g_setting       = FakeSetting{};
    g_setting.value = initial;
    g_reads         = 0;
    g_failRead      = failRead;
    return HeldSetting<std::int32_t>(SettingAccess<std::int32_t>{&FlakyRead, &Write}, kName);
}

void TestUnconfirmedApplyIsStillRestored()
{
    std::puts("a write that ran but could not be confirmed is still undone on leave");
    auto setting = FreshFlaky(0, 1);  // read 0: the current value; read 1: the confirmation fails
    EXPECT(setting.Update(true, kHeld).kind == Kind::ApplyFailed);
    EXPECT(g_setting.value == 2);     // the write did take
    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(g_setting.value == 0);
}

void TestUnreadableValueIsNotOverwrittenOnLeave()
{
    std::puts("an unreadable value on leave is not overwritten; the restore is retried");
    auto setting = FreshFlaky(0, 2);  // reads 0-1: the join; read 2: the leave's check fails
    EXPECT(setting.Update(true, kHeld).kind == Kind::Applied);
    g_setting.value = 1;              // someone else's choice, which the mod cannot see
    EXPECT(setting.Update(false, kHeld).kind == Kind::RestoreFailed);
    EXPECT(g_setting.value == 1);     // not overwritten blind
    EXPECT(g_setting.writes == 1);
}

void TestUnreadableLeaveIsRetried()
{
    std::puts("a restore blocked by an unreadable value completes at the next leave");
    auto setting = FreshFlaky(0, 2);
    setting.Update(true, kHeld);                                   // 0 -> 2
    EXPECT(setting.Update(false, kHeld).kind == Kind::RestoreFailed);
    EXPECT(g_setting.value == 2);
    EXPECT(setting.Update(true, kHeld).kind == Kind::AlreadySet);  // owed value kept
    const auto left = setting.Update(false, kHeld);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(g_setting.value == 0);
}

void TestWriteThatDidNotTakeNeedsNoRestore()
{
    std::puts("a write that ran but changed nothing leaves nothing to undo");
    auto setting = Fresh(0);
    g_setting.writeIgnored = true;
    EXPECT(setting.Update(true, kHeld).kind == Kind::ApplyFailed);
    g_setting.writeIgnored = false;
    EXPECT(setting.Update(false, kHeld).kind == Kind::None);
    EXPECT(g_setting.value == 0);
    EXPECT(g_setting.writes == 1);
}

void TestRejoinAppliesAgain()
{
    std::puts("leave then rejoin applies again");
    auto setting = Fresh(0);
    setting.Update(true, kHeld);
    setting.Update(false, kHeld);
    EXPECT(setting.Update(true, kHeld).kind == Kind::Applied);
    EXPECT(g_setting.value == 2);
}

void TestOwnWritesKeepTheFirstValue()
{
    std::puts("the mod's own later writes still go back to the first value; back at it, nothing is owed");
    auto setting = Fresh(0);
    EXPECT(setting.Hold(1).kind == Kind::Applied);  // 0 -> 1
    const auto again = setting.Hold(2);             // 1 -> 2: 1 is the mod's own write
    EXPECT(again.kind == Kind::Applied && again.value == 1 && again.held == 2);
    const auto left = setting.Release();
    EXPECT(left.kind == Kind::Restored && left.value == 0);  // not 1

    setting = Fresh(0);
    setting.Hold(1);
    const auto back = setting.Hold(0);  // back at the game's own
    EXPECT(back.kind == Kind::Applied && back.value == 1 && back.held == 0);
    EXPECT(setting.Release().kind == Kind::None);
    EXPECT(g_setting.value == 0);

    // A hold after the write back starts afresh: the game's own value is remembered again (the regression run of
    // 2026-10-22 found a layout toggled side, top, side left side by side at the leave).
    EXPECT(setting.Hold(1).kind == Kind::Applied);
    const auto third = setting.Release();
    EXPECT(third.kind == Kind::Restored && third.value == 0);
    EXPECT(g_setting.value == 0);

    setting = Fresh(0);
    setting.Hold(1);
    g_setting.value = 0;                                       // someone put the game's own back meanwhile
    EXPECT(setting.Hold(0).kind == Kind::AlreadySet);          // nothing written, nothing owed any more
    EXPECT(setting.Release().kind == Kind::None);
}

void TestReleasedValueIsNobodysWrite()
{
    std::puts("after a release, the mod's old value chosen by someone else is theirs: put back after the next hold");
    auto setting = Fresh(0);
    setting.Hold(1);
    EXPECT(setting.Release().kind == Kind::Restored);  // 0 again
    g_setting.value = 1;                               // their choice, the value the mod last wrote
    const auto held = setting.Hold(2);
    EXPECT(held.kind == Kind::Applied && held.value == 1);
    const auto left = setting.Release();
    EXPECT(left.kind == Kind::Restored && left.value == 1);
    EXPECT(g_setting.value == 1);
}

void TestEmptyValueHoldsNothing()
{
    std::puts("an empty value counts as in but holds nothing, and a later hold is released with the leave");
    auto setting = Fresh(0);
    EXPECT(setting.Update(true, std::nullopt).kind == Kind::None);
    EXPECT(setting.In());
    EXPECT(g_setting.writes == 0);
    EXPECT(setting.Hold(kHeld).kind == Kind::Applied);
    const auto left = setting.Update(false, std::nullopt);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(setting.Read() == 0);
}

void TestThreePlayerLayout()
{
    std::puts("the three-player layout: columns held while the third view is in, the game's value back after");
    g_setting       = FakeSetting{};
    g_setting.name  = L"ThreePlayerSplitscreenLayout";
    g_setting.value = 0;
    HeldSetting<std::int32_t> layout(SettingAccess<std::int32_t>{&Read, &Write}, L"ThreePlayerSplitscreenLayout");
    const auto held = layout.Hold(2);
    EXPECT(held.kind == Kind::Applied && held.value == 0 && held.held == 2);
    EXPECT(g_setting.value == 2);
    EXPECT(layout.Release().kind == Kind::Restored);
    EXPECT(g_setting.value == 0);
    EXPECT(layout.Release().kind == Kind::None);  // nothing held: a second release does nothing

    g_setting.value = 2;                          // already columns: nothing to hold, nothing to put back
    EXPECT(layout.Hold(2).kind == Kind::AlreadySet);
    EXPECT(layout.Release().kind == Kind::None);
    EXPECT(g_setting.value == 2);
}

void TestSettingsOrder()
{
    std::puts("settings: the timing method before the gap recorder on a join, after it on a leave");
    using lss::players::SettingOf;
    using lss::players::SettingsOrder;
    using lss::players::SettingsStep;
    const auto join = SettingsOrder(true);
    EXPECT(join[0] == SettingsStep::Resolution && join[1] == SettingsStep::Timing && join[2] == SettingsStep::Gap);
    const auto leave = SettingsOrder(false);
    EXPECT(leave[0] == SettingsStep::Resolution && leave[1] == SettingsStep::Gap && leave[2] == SettingsStep::Timing);
    const auto& resolution = SettingOf(SettingsStep::Resolution);
    const auto& timing     = SettingOf(SettingsStep::Timing);
    const auto& gap        = SettingOf(SettingsStep::Gap);
    EXPECT(std::wstring(resolution.name) == kName && resolution.value == 2);
    EXPECT(std::wstring(timing.name) == L"r.DynamicRes.GPUTimingMeasureMethod" && timing.value == 1);
    EXPECT(std::wstring(gap.name) == L"D3D12.EnableGapRecorder" && gap.value == 0);
}

std::wstring Named(std::int32_t value)
{
    return value == 1 ? L"1 (one)" : std::to_wstring(value);
}

void TestDescribe()
{
    std::puts("every change has text, None none; the label, the name and the values are in it");
    using lss::players::Describe;
    const wchar_t* gap = L"D3D12.EnableGapRecorder";
    EXPECT(Describe(HeldChange{Kind::None, 0, 0}, L"split screen", gap).empty());
    for (const auto kind : {Kind::Chosen, Kind::Unreadable, Kind::Applied, Kind::AlreadySet, Kind::ApplyFailed,
                            Kind::Restored, Kind::LeftAlone, Kind::RestoreFailed})
    {
        const auto text = Describe(HeldChange{kind, 1, 0}, L"split screen", gap);
        EXPECT(text.rfind(L"split screen: ", 0) == 0 && text.find(gap) != std::wstring::npos);
    }
    EXPECT(Describe(HeldChange{Kind::Applied, 1, 0}, L"split screen", gap) ==
           L"split screen: D3D12.EnableGapRecorder 1 -> 0 until the leave");
    EXPECT(Describe(HeldChange{Kind::Restored, 1, 0}, L"split screen", gap) ==
           L"split screen: D3D12.EnableGapRecorder restored to 1");
    EXPECT(Describe(HeldChange{Kind::AlreadySet, 0, 0}, L"split screen", gap) ==
           L"split screen: D3D12.EnableGapRecorder is already 0; left as it is");
    EXPECT(Describe(HeldChange{Kind::LeftAlone, 1, 0}, L"split screen", gap) ==
           L"split screen: D3D12.EnableGapRecorder was changed to 1 meanwhile; left as it is");
    EXPECT(Describe(HeldChange{Kind::ApplyFailed, 1, 0}, L"split screen", gap).find(L"could not set") !=
           std::wstring::npos);
    EXPECT(Describe(HeldChange{Kind::RestoreFailed, 1, 0}, L"split screen", gap).find(L"could not restore") !=
           std::wstring::npos);
    // A value's text: the layouts name theirs.
    EXPECT(Describe(HeldChange{Kind::Applied, 0, 1}, L"third view", L"Layout", &Named) ==
           L"third view: Layout 0 -> 1 (one) until the leave");
    EXPECT(Describe(HeldChange{Kind::Chosen, 0, 1}, L"split layout", L"Layout", &Named) ==
           L"split layout: Layout 1 (one) from the next join");
}

}  // namespace

int main()
{
    TestJoinSetsAndLeaveRestores();
    TestAlreadySetIsNotRestored();
    TestSomeoneElsesChoiceIsKept();
    TestFailuresAreReported();
    TestFailedRestoreIsRetriedAtTheNextLeave();
    TestChoiceAfterFailedRestoreIsWhatComesBack();
    TestUnconfirmedApplyIsStillRestored();
    TestUnreadableValueIsNotOverwrittenOnLeave();
    TestUnreadableLeaveIsRetried();
    TestWriteThatDidNotTakeNeedsNoRestore();
    TestRejoinAppliesAgain();
    TestOwnWritesKeepTheFirstValue();
    TestReleasedValueIsNobodysWrite();
    TestEmptyValueHoldsNothing();
    TestThreePlayerLayout();
    TestSettingsOrder();
    TestDescribe();

    return lss::test::Report();
}
