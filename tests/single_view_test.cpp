// Host-side tests for players::SingleView, run without the game.
//
// The switch is a fake bool. The cases: nothing happens without player 2 or without a whole-screen
// state; the mod sets the switch and clears only its own; a switch found set is never cleared; a
// switch cleared by someone else is not written again; failures reported once per request and
// retried when the request changes; the development modes; player 2 leaving clears the switch.

#include "players/single_view.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using lss::players::SingleView;
using lss::players::SingleViewChange;
using lss::players::SingleViewMode;
using lss::players::SplitSwitchAccess;
using Kind = SingleViewChange::Kind;

struct FakeSwitch
{
    bool disabled   = false;
    bool readWorks  = true;
    bool writeWorks = true;
    int  writes     = 0;
} g_switch;

std::optional<bool> Read() noexcept
{
    return g_switch.readWorks ? std::optional<bool>(g_switch.disabled) : std::nullopt;
}

bool Write(bool disabled) noexcept
{
    ++g_switch.writes;
    if (!g_switch.writeWorks)
    {
        return false;
    }
    g_switch.disabled = disabled;
    return true;
}

SingleView Fresh()
{
    g_switch = FakeSwitch{};
    return SingleView(SplitSwitchAccess{&Read, &Write});
}

void TestNothingWithoutReason()
{
    std::puts("single view: no player 2, or no whole-screen state, never touches the switch");
    auto view = Fresh();
    EXPECT(view.Update(false, false).kind == Kind::None);
    EXPECT(view.Update(false, true).kind == Kind::None);  // a menu with one player: the game's own screen
    EXPECT(view.Update(true, false).kind == Kind::None);
    EXPECT(g_switch.writes == 0);
    EXPECT(!view.Single());
}

void TestSetAndClearOwn()
{
    std::puts("single view: set for a whole-screen state, cleared when it ends, once each");
    auto view = Fresh();
    EXPECT(view.Update(true, true).kind == Kind::Single);
    EXPECT(g_switch.disabled && view.Single());
    EXPECT(view.Update(true, true).kind == Kind::None);  // held: not written again
    EXPECT(g_switch.writes == 1);

    EXPECT(view.Update(true, false).kind == Kind::Split);
    EXPECT(!g_switch.disabled && !view.Single());
    EXPECT(view.Update(true, false).kind == Kind::None);
    EXPECT(g_switch.writes == 2);
}

void TestPlayerTwoLeavesWhileSingle()
{
    std::puts("single view: player 2 leaving during a whole-screen state clears the switch");
    auto view = Fresh();
    view.Update(true, true);
    EXPECT(view.Update(false, true).kind == Kind::Split);
    EXPECT(!g_switch.disabled);
}

void TestFoundSetIsNotCleared()
{
    std::puts("single view: a switch already set is used but never cleared by the mod");
    auto view = Fresh();
    g_switch.disabled = true;
    EXPECT(view.Update(true, true).kind == Kind::AlreadySingle);
    EXPECT(view.Single());
    EXPECT(view.Update(true, false).kind == Kind::LeftAlone);
    EXPECT(g_switch.disabled && g_switch.writes == 0);
    EXPECT(!view.Single());
}

void TestClearedBySomeoneElse()
{
    std::puts("single view: a switch someone else cleared meanwhile is not written again");
    auto view = Fresh();
    view.Update(true, true);
    g_switch.disabled = false;
    const int writes = g_switch.writes;
    EXPECT(view.Update(true, false).kind == Kind::LeftAlone);
    EXPECT(g_switch.writes == writes && !view.Single());
    EXPECT(view.Update(true, true).kind == Kind::Single);  // the next request is the mod's again
}

void TestFailuresReportedOncePerRequest()
{
    std::puts("single view: failures reported once per request, retried when the request changes");
    auto view = Fresh();
    g_switch.readWorks = false;
    EXPECT(view.Update(true, true).kind == Kind::SingleFailed);
    EXPECT(view.Update(true, true).kind == Kind::None);  // not retried every tick
    EXPECT(g_switch.writes == 0 && !view.Single());
    g_switch.readWorks = true;
    EXPECT(view.Update(true, false).kind == Kind::None);  // the split is what is on screen
    EXPECT(view.Update(true, true).kind == Kind::Single);  // a new request is tried afresh

    view = Fresh();
    g_switch.writeWorks = false;
    EXPECT(view.Update(true, true).kind == Kind::SingleFailed);
    EXPECT(!g_switch.disabled && !view.Single());

    view = Fresh();
    view.Update(true, true);
    g_switch.writeWorks = false;
    EXPECT(view.Update(true, false).kind == Kind::SplitFailed);
    EXPECT(view.Single());  // the screen is still single: the presentation says so
    EXPECT(view.Update(true, false).kind == Kind::None);
    g_switch.writeWorks = true;
    EXPECT(view.Update(true, true).kind == Kind::None);  // wanted single, and it is
    EXPECT(view.Update(true, false).kind == Kind::Split);  // the next request clears it
    EXPECT(!g_switch.disabled);

    view = Fresh();
    view.Update(true, true);
    g_switch.readWorks = false;
    EXPECT(view.Update(false, false).kind == Kind::SplitFailed);
    EXPECT(g_switch.disabled);  // unknown: not overwritten
}

void TestModes()
{
    std::puts("single view: the development modes override the automatic choice while player 2 is in");
    auto view = Fresh();
    view.SetMode(SingleViewMode::Single);
    EXPECT(view.Update(true, false).kind == Kind::Single);
    view.SetMode(SingleViewMode::Split);
    EXPECT(view.Update(true, true).kind == Kind::Split);
    view.SetMode(SingleViewMode::Auto);
    EXPECT(view.Update(true, true).kind == Kind::Single);
    view.SetMode(SingleViewMode::Single);
    EXPECT(view.Update(false, true).kind == Kind::Split);  // no player 2: never single
}

void TestWantedAndDescribe()
{
    std::puts("single view: wanted only in auto mode, for the whole screen, a wait or standing aside; each change has text");
    using lss::players::Describe;
    using lss::players::SingleViewWanted;
    EXPECT(!SingleViewWanted(true, false, false, false));
    EXPECT(SingleViewWanted(true, true, false, false));
    EXPECT(SingleViewWanted(true, false, true, false));
    EXPECT(SingleViewWanted(true, false, false, true));
    EXPECT(!SingleViewWanted(false, true, true, true));
    EXPECT(Describe(SingleViewChange{Kind::None}).empty());
    for (const auto kind : {Kind::Single, Kind::AlreadySingle, Kind::SingleFailed, Kind::Split, Kind::LeftAlone, Kind::SplitFailed})
    {
        EXPECT(!Describe(SingleViewChange{kind}).empty());
    }
    EXPECT(Describe(SingleViewChange{Kind::Single}).find(L"player 1's view alone") != std::wstring::npos);
    EXPECT(Describe(SingleViewChange{Kind::Split}).find(L"the split is back") != std::wstring::npos);
}

void TestInputsAndWaits()
{
    std::puts("single view: the request from its inputs; the waits edge noted once each way");
    using lss::players::DescribeWaits;
    using lss::players::SingleViewInputs;
    using Waits = SingleViewChange::Waits;
    auto view   = Fresh();
    auto change = view.Update(true, SingleViewInputs{.waitsForMember = true});
    EXPECT(change.kind == Kind::Single);
    EXPECT(change.waits == Waits::Began);
    EXPECT(DescribeWaits(change).find(L"waits for a party member") != std::wstring::npos);
    change = view.Update(true, SingleViewInputs{.waitsForMember = true});
    EXPECT(change.kind == Kind::None);
    EXPECT(change.waits == Waits::Same);
    EXPECT(DescribeWaits(change).empty());
    change = view.Update(true, SingleViewInputs{.standsAside = true});  // the wait over, another reason holds
    EXPECT(change.kind == Kind::None);
    EXPECT(change.waits == Waits::Ended);
    change = view.Update(true, SingleViewInputs{});
    EXPECT(change.kind == Kind::Split);
    EXPECT(change.waits == Waits::Same);
    view.SetMode(SingleViewMode::Split);  // not automatic: a wait is not noted
    change = view.Update(true, SingleViewInputs{.waitsForMember = true});
    EXPECT(change.kind == Kind::None);
    EXPECT(change.waits == Waits::Same);
}

}  // namespace

int main()
{
    TestNothingWithoutReason();
    TestSetAndClearOwn();
    TestPlayerTwoLeavesWhileSingle();
    TestFoundSetIsNotCleared();
    TestClearedBySomeoneElse();
    TestFailuresReportedOncePerRequest();
    TestModes();
    TestWantedAndDescribe();
    TestInputsAndWaits();

    return lss::test::Report();
}
