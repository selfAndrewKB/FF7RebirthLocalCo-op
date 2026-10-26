// Host-side tests for players::SingleView, run without the game.
//
// The switch is a fake bool. The cases: nothing happens while the single view is not wanted; the mod sets the
// switch and clears only its own; a switch found set is never cleared; a switch cleared by someone else is not
// written again; failures reported once per request and retried when the request changes. Which moments want the
// single view is views/column_source.h's (tests/column_source_test.cpp).

#include "players/single_view.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using lss::players::SingleView;
using lss::players::SingleViewChange;
using lss::players::SplitSwitchAccess;

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

void TestNothingWhileNotWanted()
{
    std::puts("single view: not wanted: never touches the switch");
    auto view = Fresh();
    EXPECT(view.Update(false) == SingleViewChange::None);
    EXPECT(view.Update(false) == SingleViewChange::None);
    EXPECT(g_switch.writes == 0);
    EXPECT(!view.Single());
}

void TestSetAndClearOwn()
{
    std::puts("single view: set when wanted, cleared when no longer, once each");
    auto view = Fresh();
    EXPECT(view.Update(true) == SingleViewChange::Single);
    EXPECT(g_switch.disabled && view.Single());
    EXPECT(view.Update(true) == SingleViewChange::None);  // held: not written again
    EXPECT(g_switch.writes == 1);

    EXPECT(view.Update(false) == SingleViewChange::Split);
    EXPECT(!g_switch.disabled && !view.Single());
    EXPECT(view.Update(false) == SingleViewChange::None);
    EXPECT(g_switch.writes == 2);
}

void TestFoundSetIsNotCleared()
{
    std::puts("single view: a switch already set is used but never cleared by the mod");
    auto view = Fresh();
    g_switch.disabled = true;
    EXPECT(view.Update(true) == SingleViewChange::AlreadySingle);
    EXPECT(view.Single());
    EXPECT(view.Update(false) == SingleViewChange::LeftAlone);
    EXPECT(g_switch.disabled && g_switch.writes == 0);
    EXPECT(!view.Single());
}

void TestClearedBySomeoneElse()
{
    std::puts("single view: a switch someone else cleared meanwhile is not written again");
    auto view = Fresh();
    view.Update(true);
    g_switch.disabled = false;
    const int writes = g_switch.writes;
    EXPECT(view.Update(false) == SingleViewChange::LeftAlone);
    EXPECT(g_switch.writes == writes && !view.Single());
    EXPECT(view.Update(true) == SingleViewChange::Single);  // the next request is the mod's again
}

void TestFailuresReportedOncePerRequest()
{
    std::puts("single view: failures reported once per request, retried when the request changes");
    auto view = Fresh();
    g_switch.readWorks = false;
    EXPECT(view.Update(true) == SingleViewChange::SingleFailed);
    EXPECT(view.Update(true) == SingleViewChange::None);  // not retried every tick
    EXPECT(g_switch.writes == 0 && !view.Single());
    g_switch.readWorks = true;
    EXPECT(view.Update(false) == SingleViewChange::None);  // the split is what is on screen
    EXPECT(view.Update(true) == SingleViewChange::Single);  // a new request is tried afresh

    view = Fresh();
    g_switch.writeWorks = false;
    EXPECT(view.Update(true) == SingleViewChange::SingleFailed);
    EXPECT(!g_switch.disabled && !view.Single());

    view = Fresh();
    view.Update(true);
    g_switch.writeWorks = false;
    EXPECT(view.Update(false) == SingleViewChange::SplitFailed);
    EXPECT(view.Single());  // the screen is still single: the presentation says so
    EXPECT(view.Update(false) == SingleViewChange::None);
    g_switch.writeWorks = true;
    EXPECT(view.Update(true) == SingleViewChange::None);  // wanted single, and it is
    EXPECT(view.Update(false) == SingleViewChange::Split);  // the next request clears it
    EXPECT(!g_switch.disabled);

    view = Fresh();
    view.Update(true);
    g_switch.readWorks = false;
    EXPECT(view.Update(false) == SingleViewChange::SplitFailed);
    EXPECT(g_switch.disabled);  // unknown: not overwritten
}

void TestDescribe()
{
    std::puts("single view: each change has text");
    using lss::players::Describe;
    EXPECT(Describe(SingleViewChange::None).empty());
    for (const auto change : {SingleViewChange::Single, SingleViewChange::AlreadySingle, SingleViewChange::SingleFailed,
                              SingleViewChange::Split, SingleViewChange::LeftAlone, SingleViewChange::SplitFailed})
    {
        EXPECT(!Describe(change).empty());
    }
    EXPECT(Describe(SingleViewChange::Single).find(L"player 1's view alone") != std::wstring::npos);
    EXPECT(Describe(SingleViewChange::Split).find(L"the split is back") != std::wstring::npos);
}

}  // namespace

int main()
{
    TestNothingWhileNotWanted();
    TestSetAndClearOwn();
    TestFoundSetIsNotCleared();
    TestClearedBySomeoneElse();
    TestFailuresReportedOncePerRequest();
    TestDescribe();

    return lss::test::Report();
}
