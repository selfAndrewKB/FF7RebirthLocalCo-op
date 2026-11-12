// Host-side tests for session/state_path.h: a member's chain of level states, walked as the lead gate walks it. The
// chain shapes are the other game's live ones (Aerith's guide home, Barret's walk-ups to a terminal) and this game's
// own (a setting group, a chain past the limit), with the destinations made up.

#include "session/state_path.h"

#include "expect.h"

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace sp = lss::session::state_path;

using Rows = std::map<std::wstring, sp::Row>;

sp::Walk WalkIn(const Rows& rows, const std::wstring& first, std::size_t maxLinks = 16)
{
    return sp::WalkFrom(
        first,
        [&rows](const std::wstring& name) -> std::optional<sp::Row> {
            const auto it = rows.find(name);
            return it == rows.end() ? std::nullopt : std::optional<sp::Row>(it->second);
        },
        maxLinks);
}

void AGuideHome()
{
    std::puts("a guide: run, stay, run: a further move at the second link; every destination kept");
    const Rows rows = {
        {L"Lead_00", {L"Stay", {L"Lead_01"}, 0}},
        {L"Lead_01", {L"MoveToTarget", {L"Town5"}, 101}},
        {L"Town5", {L"", {L"PartyOut"}, 0}},
        {L"PartyOut", {L"Stay", {L"Wait"}, 0}},
    };
    const sp::Walk walk = WalkIn(rows, L"Lead_00");
    EXPECT(walk.firstMove.has_value() && *walk.firstMove == 1);
    EXPECT(walk.links[1].destination == 101 && walk.links[1].move);
    EXPECT(sp::FurtherMove(walk));
    EXPECT((sp::Trail(walk, 1) == std::vector<std::size_t>{0, 1}));
    // The nameless state (no Status) is the release: the path ends there, nothing past it walked.
    EXPECT(walk.links.size() == 3 && walk.links[2].read && walk.links[2].status.empty());
}

void WalkUps()
{
    std::puts("walk-ups: a run to a terminal, a stay, a run near it: a further move");
    const Rows rows = {
        {L"SearchTerminal", {L"Stay", {L"MoveToNearTerminal"}, 0}},
        {L"MoveToNearTerminal", {L"MoveToTarget", {L"WaitNearTerminal"}, 201}},
        {L"WaitNearTerminal", {L"Stay", {}, 0}},
    };
    const sp::Walk first = WalkIn(rows, L"SearchTerminal");
    EXPECT(first.firstMove.has_value() && *first.firstMove == 1);
    const sp::Walk second = WalkIn(rows, L"WaitNearTerminal");
    EXPECT(!sp::FurtherMove(second));
}

void AStandEndsThePath()
{
    std::puts("a stand ends the path: a move after a Stable is a later script's");
    const Rows rows = {
        {L"Stand", {L"Stable", {L"LaterRun"}, 0}},
        {L"Pause", {L"Stay", {L"Stand"}, 0}},
        {L"LaterRun", {L"MoveToTarget", {}, 301}},
    };
    EXPECT(!sp::FurtherMove(WalkIn(rows, L"Stand")));
    const sp::Walk pause = WalkIn(rows, L"Pause");
    EXPECT(!sp::FurtherMove(pause) && pause.links.size() == 2);
    const Rows lower = {{L"Stand", {L"stable", {L"LaterRun"}, 0}}, {L"LaterRun", {L"Lead", {}, 0}}};
    EXPECT(!sp::FurtherMove(WalkIn(lower, L"Stand")));
}

void Groups()
{
    std::puts("groups: a moving group is a further move; a following group ends the path");
    const Rows rows = {
        {L"Wait", {L"Stay", {L"ToWindow"}, 0}},
        {L"ToWindow", {L"AIStateGroup", {}, 0, true, false}},
        {L"Rest", {L"Stay", {L"Back"}, 0}},
        {L"Back", {L"AIStateGroup", {L"LaterRun"}, 0, false, true}},
        {L"LaterRun", {L"MoveToTarget", {}, 401}},
        {L"Look", {L"Stay", {L"LookGroup"}, 0}},
        {L"LookGroup", {L"AIStateGroup", {}, 0, false, false}},
    };
    const sp::Walk moves = WalkIn(rows, L"Wait");
    EXPECT(moves.firstMove.has_value() && moves.links[*moves.firstMove].name == L"ToWindow");
    EXPECT(!sp::FurtherMove(WalkIn(rows, L"Rest")));
    EXPECT(!sp::FurtherMove(WalkIn(rows, L"Look")));
}

void TheFirstStateMayBeAMove()
{
    std::puts("the arrival may lead straight to another run");
    const Rows rows = {{L"Run2", {L"Lead", {}, 501}}};
    const sp::Walk walk = WalkIn(rows, L"Run2");
    EXPECT(walk.firstMove.has_value() && *walk.firstMove == 0);
    EXPECT((sp::Trail(walk, 0) == std::vector<std::size_t>{0}));
}

void ALoopEnds()
{
    std::puts("a loop of stays ends with each state once");
    const Rows rows = {{L"A", {L"Stay", {L"B"}, 0}}, {L"B", {L"Stay", {L"A", L"B", L""}, 0}}};
    const sp::Walk walk = WalkIn(rows, L"A");
    EXPECT(walk.links.size() == 2 && !walk.truncated && !sp::FurtherMove(walk));
}

void TheLimitReadsAsAMove()
{
    std::puts("past the limit: the walk stops, and the chain still going reads as a further move (the safe side)");
    Rows   rows;
    sp::Row hub{L"Stay", {}, 0};
    for (int i = 0; i < 20; ++i)
    {
        hub.next.push_back(L"Leaf" + std::to_wstring(i));
        rows[L"Leaf" + std::to_wstring(i)] = {L"Stay", {}, 0};
    }
    rows[L"Hub"]        = hub;
    const sp::Walk walk = WalkIn(rows, L"Hub", 16);
    EXPECT(walk.links.size() == 16 && walk.truncated);
    EXPECT(!walk.firstMove.has_value());
    EXPECT(sp::FurtherMove(walk));
}

void UnknownStates()
{
    std::puts("unknown states: one unread link, no move; an empty name walks nothing");
    const Rows     rows;
    const sp::Walk unknown = WalkIn(rows, L"Nowhere");
    EXPECT(unknown.links.size() == 1 && !unknown.links[0].read && !sp::FurtherMove(unknown));
    EXPECT(WalkIn(rows, L"").links.empty());
    EXPECT(WalkIn(rows, L"Nowhere", 0).links.empty());
    EXPECT(sp::Trail(unknown, 5).empty());
}

void TheFirstMoveIsBreadthFirst()
{
    std::puts("the nearer of two moves is the one named, whatever the trigger order");
    const Rows rows = {
        {L"Fork", {L"Stay", {L"Far1", L"Near"}, 0}},
        {L"Far1", {L"Stay", {L"Far2"}, 0}},
        {L"Far2", {L"Lead", {}, 601}},
        {L"Near", {L"RouteMove", {}, 602}},
    };
    const sp::Walk walk = WalkIn(rows, L"Fork");
    EXPECT(walk.firstMove.has_value() && walk.links[*walk.firstMove].name == L"Near");
    EXPECT(walk.links.size() == 4);
}

}  // namespace

int main()
{
    AGuideHome();
    WalkUps();
    AStandEndsThePath();
    Groups();
    TheFirstStateMayBeAMove();
    ALoopEnds();
    TheLimitReadsAsAMove();
    UnknownStates();
    TheFirstMoveIsBreadthFirst();
    return lss::test::Report();
}
