// Host-side tests for platform/game_thread_sites, run without the game.
//
// The queue of 1.005 placements: runs what was queued, in order, once; refuses a null and one entry past its
// capacity; a placement that queues another during the run (the queue is copied out first) sees it run at the
// next tick; a placement queued with a context gets it.

#include "platform/game_thread_sites.h"

#include "expect.h"

#include <cstdio>
#include <vector>

namespace {

using lss::platform::game_thread_sites::kCapacity;
using lss::platform::game_thread_sites::Queue;
using lss::platform::game_thread_sites::Run;

std::vector<int> g_ran;

void PlaceA() noexcept
{
    g_ran.push_back(1);
}
void PlaceB() noexcept
{
    g_ran.push_back(2);
}
void PlaceWith(void* context) noexcept
{
    g_ran.push_back(*static_cast<const int*>(context));
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
    std::puts("game thread sites: a null is refused, and so is one placement past the capacity (the others still run)");
    g_ran.clear();
    EXPECT(!Queue(nullptr));
    for (std::size_t i = 0; i < kCapacity; ++i)
    {
        EXPECT(Queue(&PlaceA));
    }
    EXPECT(!Queue(&PlaceB));
    Run();
    EXPECT(g_ran.size() == kCapacity);
    for (int ran : g_ran)
    {
        EXPECT(ran == 1);
    }
}

void TestContext()
{
    std::puts("game thread sites: a placement queued with a context runs with it, in order with the others");
    g_ran.clear();
    int seven = 7;
    EXPECT(Queue(&PlaceA));
    EXPECT(Queue(&PlaceWith, &seven));
    EXPECT(!Queue(static_cast<void (*)(void*) noexcept>(nullptr), &seven));
    Run();
    EXPECT((g_ran == std::vector<int>{1, 7}));
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
    TestContext();
    TestQueueDuringRun();

    return lss::test::Report();
}
