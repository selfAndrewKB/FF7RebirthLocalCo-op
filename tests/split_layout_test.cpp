// Host-side tests for players::SplitLayout, run without the game.
//
// The setting is a fake byte. The cases: the game's layout untouched until a layout is chosen; a
// choice made with no other player in applied at the join; a switch with one in applied at once;
// the game's value put back on leave; switching back to the game's own leaves nothing to restore;
// a value someone else set left alone; failures reported rather than assumed. The hold itself is
// players::HeldSetting's (held_setting_test.cpp); here, the choice over it.

#include "players/split_layout.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using lss::players::HeldChange;
using lss::players::SettingAccess;
using lss::players::SplitLayout;
using Kind = HeldChange::Kind;

constexpr std::uint8_t kTopBottom  = 0;
constexpr std::uint8_t kSideBySide = 1;

struct FakeSetting
{
    std::uint8_t value      = kTopBottom;
    bool         readWorks  = true;
    bool         writeWorks = true;
    int          writes     = 0;
} g_setting;

std::optional<std::uint8_t> Read(const wchar_t*) noexcept
{
    return g_setting.readWorks ? std::optional<std::uint8_t>(g_setting.value) : std::nullopt;
}

bool Write(const wchar_t*, std::uint8_t value) noexcept
{
    ++g_setting.writes;
    if (!g_setting.writeWorks)
    {
        return false;
    }
    g_setting.value = value;
    return true;
}

SplitLayout Fresh(std::uint8_t gameValue)
{
    g_setting       = FakeSetting{};
    g_setting.value = gameValue;
    return SplitLayout(SettingAccess<std::uint8_t>{&Read, &Write});
}

void TestGameLayoutUntilChosen()
{
    std::puts("layout: nothing chosen, the game's layout is never written");
    auto layout = Fresh(kTopBottom);
    EXPECT(layout.Update(false).kind == Kind::None);
    EXPECT(layout.Update(true).kind == Kind::None);
    EXPECT(layout.Update(false).kind == Kind::None);
    EXPECT(g_setting.writes == 0);
}

void TestChoiceAppliedAtJoinAndRestored()
{
    std::puts("layout: chosen without player 2, applied at the join, the game's put back on leave");
    auto layout = Fresh(kTopBottom);
    const auto chosen = layout.Toggle();
    EXPECT(chosen.kind == Kind::Chosen && chosen.held == kSideBySide);
    EXPECT(g_setting.writes == 0);  // no other player: nothing written yet

    const auto joined = layout.Update(true);
    EXPECT(joined.kind == Kind::Applied && joined.held == kSideBySide && joined.value == kTopBottom);
    EXPECT(g_setting.value == kSideBySide);

    const auto left = layout.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == kTopBottom);
    EXPECT(g_setting.value == kTopBottom);

    // The choice survives the leave: the next join applies it again.
    EXPECT(layout.Update(true).kind == Kind::Applied);
    EXPECT(g_setting.value == kSideBySide);
}

void TestSwitchWithPlayerTwoIn()
{
    std::puts("layout: a switch with player 2 in applies at once; switching back leaves nothing to restore");
    auto layout = Fresh(kTopBottom);
    layout.Update(true);
    EXPECT(layout.Toggle().kind == Kind::Applied);
    EXPECT(g_setting.value == kSideBySide);

    const auto back = layout.Toggle();
    EXPECT(back.kind == Kind::Applied && back.held == kTopBottom);
    const int writes = g_setting.writes;
    EXPECT(layout.Update(false).kind == Kind::None);  // the game's value is already in place
    EXPECT(g_setting.writes == writes);

    // Side, top, side, leave: the game's layout comes back (the regression's own sequence).
    layout.Update(true);
    EXPECT(layout.Toggle().kind == Kind::Applied);
    EXPECT(layout.Toggle().kind == Kind::Applied);
    EXPECT(layout.Toggle().kind == Kind::Applied);
    EXPECT(g_setting.value == kSideBySide);
    const auto left = layout.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == kTopBottom);
    EXPECT(g_setting.value == kTopBottom);
}

void TestChooseAsksForAState()
{
    std::puts("layout: Choose sets a layout whatever is on screen; choosing it again changes nothing");
    auto layout = Fresh(kTopBottom);
    const auto chosen = layout.Choose(kSideBySide);
    EXPECT(chosen.kind == Kind::Chosen && chosen.held == kSideBySide);
    EXPECT(g_setting.writes == 0);

    EXPECT(layout.Update(true).kind == Kind::Applied);
    EXPECT(g_setting.value == kSideBySide);
    const int writes = g_setting.writes;
    const auto again = layout.Choose(kSideBySide);
    EXPECT(again.kind == Kind::AlreadySet && again.held == kSideBySide);
    EXPECT(g_setting.writes == writes);  // already in place: not written again

    EXPECT(layout.Choose(kTopBottom).kind == Kind::Applied);
    EXPECT(g_setting.value == kTopBottom);
    EXPECT(layout.Update(false).kind == Kind::None);  // the game's own value is in place
}

void TestGameAlreadySideBySide()
{
    std::puts("layout: the switch starts from the game's value when nothing is chosen");
    auto layout = Fresh(kSideBySide);
    layout.Update(true);
    const auto change = layout.Toggle();
    EXPECT(change.kind == Kind::Applied && change.held == kTopBottom);
    EXPECT(layout.Update(false).kind == Kind::Restored);
    EXPECT(g_setting.value == kSideBySide);
}

void TestSomeoneElseChangedIt()
{
    std::puts("layout: a value set by someone else after the mod is left alone on leave");
    auto layout = Fresh(kTopBottom);
    layout.Toggle();
    layout.Update(true);
    g_setting.value = 7;  // not a value the mod writes
    const auto left = layout.Update(false);
    EXPECT(left.kind == Kind::LeftAlone && left.value == 7);
    EXPECT(g_setting.value == 7);
}

void TestFailuresReported()
{
    std::puts("layout: unreadable or unwritable settings are reported; an unknown value is not overwritten");
    auto layout = Fresh(kTopBottom);
    layout.Toggle();
    g_setting.readWorks = false;
    EXPECT(layout.Update(true).kind == Kind::ApplyFailed);
    EXPECT(g_setting.writes == 0);

    layout = Fresh(kTopBottom);
    layout.Toggle();
    g_setting.writeWorks = false;
    EXPECT(layout.Update(true).kind == Kind::ApplyFailed);
    EXPECT(g_setting.value == kTopBottom);
    EXPECT(layout.Update(false).kind == Kind::None);  // nothing was changed, so nothing is owed

    layout = Fresh(kTopBottom);
    layout.Toggle();
    layout.Update(true);
    g_setting.readWorks = false;
    const auto left = layout.Update(false);
    EXPECT(left.kind == Kind::RestoreFailed && left.value == kTopBottom);
    EXPECT(g_setting.value == kSideBySide);  // unknown: not overwritten
    g_setting.readWorks = true;
    layout.Update(true);  // a player back: the chosen layout is still in place
    EXPECT(layout.Update(false).kind == Kind::Restored);  // retried at the next leave
    EXPECT(g_setting.value == kTopBottom);

    layout = Fresh(kTopBottom);
    layout.Toggle();
    layout.Update(true);
    g_setting.writeWorks = false;
    const auto unwritten = layout.Update(false);
    EXPECT(unwritten.kind == Kind::RestoreFailed && unwritten.value == kTopBottom);
    g_setting.writeWorks = true;
    layout.Update(true);
    EXPECT(layout.Update(false).kind == Kind::Restored);  // retried at the next leave
    EXPECT(g_setting.value == kTopBottom);

    layout = Fresh(kTopBottom);
    g_setting.readWorks = false;
    EXPECT(layout.Toggle().kind == Kind::Unreadable);  // no guess: nothing chosen
    g_setting.readWorks = true;
    EXPECT(layout.Update(true).kind == Kind::None);
    EXPECT(g_setting.writes == 0);
}

void TestDescribe()
{
    std::puts("layout: every change has text, None none; the lines name the layout");
    using lss::players::Describe;
    using lss::players::LayoutText;
    const auto layout = Fresh(kTopBottom);
    EXPECT(Describe(layout, HeldChange{Kind::None, 0, 0}).empty());
    for (const auto kind : {Kind::Chosen, Kind::Unreadable, Kind::Applied, Kind::AlreadySet, Kind::ApplyFailed,
                            Kind::Restored, Kind::LeftAlone, Kind::RestoreFailed})
    {
        EXPECT(!Describe(layout, HeldChange{kind, kTopBottom, kSideBySide}).empty());
    }
    EXPECT(Describe(layout, HeldChange{Kind::Applied, kTopBottom, kSideBySide}) ==
           L"split layout: TwoPlayerSplitscreenLayout 0 (top/bottom) -> 1 (side by side) until the leave");
    EXPECT(Describe(layout, HeldChange{Kind::Restored, kTopBottom, kSideBySide}) ==
           L"split layout: TwoPlayerSplitscreenLayout restored to 0 (top/bottom)");
    EXPECT(Describe(layout, HeldChange{Kind::Chosen, 0, 7}).find(L"7 (another value) from the next join") !=
           std::wstring::npos);
    EXPECT(LayoutText(1) == L"1 (side by side)");
}

}  // namespace

int main()
{
    TestGameLayoutUntilChosen();
    TestChoiceAppliedAtJoinAndRestored();
    TestSwitchWithPlayerTwoIn();
    TestChooseAsksForAState();
    TestGameAlreadySideBySide();
    TestSomeoneElseChangedIt();
    TestFailuresReported();
    TestDescribe();

    return lss::test::Report();
}
