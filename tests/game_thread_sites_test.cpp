// Host-side tests for player_two::game_thread_sites, run without the game.
//
// The queue of 1.005 placements: runs what was queued, in order, once; refuses a null and a seventeenth entry; a
// placement that queues another during the run (the queue is copied out first) sees it run at the next tick.

#include "player_two/game_thread_sites.h"

#include "expect.h"

#include <cstdio>
#include <vector>

namespace {

using lss::player_two::game_thread_sites::Queue;
using lss::player_two::game_thread_sites::Run;

std::vector<int> g_ran;

void PlaceA() noexcept
{
    g_ran.push_back(1);
}
void PlaceB() noexcept
{
    g_ran.push_back(2);
}
void PlaceAndRequeue() noexcept
{
    g_ran.push_back(3);
    Queue(&PlaceA);
}

void TestRunsInOrderOnce()
{
    std::puts("game thread sites: queued placements run in order, once; an empty queue runs nothing");
    g_ran.clear();
    Run();
    EXPECT(g_ran.empty());
    EXPECT(Queue(&PlaceA));
    EXPECT(Queue(&PlaceB));
    EXPECT(g_ran.empty());  // nothing runs at queue time
    Run();
    EXPECT((g_ran == std::vector<int>{1, 2}));
    Run();
    EXPECT((g_ran == std::vector<int>{1, 2}));
}

void TestRefusals()
{
    std::puts("game thread sites: a null is refused, and so is a seventeenth placement (the first sixteen still run)");
    g_ran.clear();
    EXPECT(!Queue(nullptr));
    for (int i = 0; i < 16; ++i)
    {
        EXPECT(Queue(&PlaceA));
    }
    EXPECT(!Queue(&PlaceB));
    Run();
    EXPECT(g_ran.size() == 16);
    for (int ran : g_ran)
    {
        EXPECT(ran == 1);
    }
}

void TestQueueDuringRun()
{
    std::puts("game thread sites: a placement queued during a run waits for the next one");
    g_ran.clear();
    EXPECT(Queue(&PlaceAndRequeue));
    Run();
    EXPECT((g_ran == std::vector<int>{3}));
    Run();
    EXPECT((g_ran == std::vector<int>{3, 1}));
    Run();
    EXPECT((g_ran == std::vector<int>{3, 1}));
}

}  // namespace

int main()
{
    TestRunsInOrderOnce();
    TestRefusals();
    TestQueueDuringRun();

    return lss::test::Report();
}
