// Host-side tests for player_two::rumble_rules, run without the game.
//
// The copied-key set, which of player 1's loops a shared ride seeds, where an add and a stop go, and whose list the
// component's tick plays.

#include "player_two/rumble_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::player_two::rumble_rules;

void TestKeySet()
{
    std::puts("key set: unique inserts up to the capacity; an erase swaps the last in; clear");
    KeySet<4> keys;
    EXPECT(keys.Count() == 0);
    EXPECT(keys.Insert(10) && keys.Insert(20) && keys.Insert(30));
    EXPECT(!keys.Insert(20));  // present
    EXPECT(keys.Count() == 3);
    EXPECT(keys.Insert(40));
    EXPECT(!keys.Insert(50));  // full
    EXPECT(keys.Count() == 4);
    EXPECT(keys.Erase(20));
    EXPECT(keys.Count() == 3);
    EXPECT(keys.Keys()[1] == 40);  // the last took its place
    EXPECT(!keys.Erase(20));
    EXPECT(!keys.Erase(99));
    EXPECT(keys.Insert(50));
    keys.Clear();
    EXPECT(keys.Count() == 0);
    EXPECT(!keys.Erase(10));
    EXPECT(keys.Insert(10));
}

void TestSeedsLoop()
{
    std::puts("seed: loops that are not ending and not in states 6 or 7");
    EXPECT(SeedsLoop(kFlagLoop, 0));
    EXPECT(SeedsLoop(kFlagLoop | 0x01, 3));
    EXPECT(!SeedsLoop(0, 0));     // a one-shot
    EXPECT(!SeedsLoop(0x01, 0));  // not a loop
    for (const std::uint8_t ending : {0x04, 0x08, 0x10})
    {
        EXPECT(!SeedsLoop(static_cast<std::uint8_t>(kFlagLoop | ending), 0));
    }
    EXPECT(!SeedsLoop(kFlagLoop, 6));
    EXPECT(!SeedsLoop(kFlagLoop, 7));
    EXPECT(SeedsLoop(kFlagLoop, 5));
}

void TestRoutes()
{
    std::puts("routes: player 2's list when its own and taken; else the game's, copied during a shared ride; a stop of "
              "player 2's on a worker is left");
    EXPECT(AddRouteOf(true, true, true) == Route::Two);
    EXPECT(AddRouteOf(true, false, true) == Route::Game);  // player 2's, but the list did not take it: never copied
    EXPECT(AddRouteOf(false, false, false) == Route::Game);
    EXPECT(AddRouteOf(false, false, true) == Route::GameAndCopy);

    EXPECT(StopRouteOf(true, false, true, true, false, true) == Route::LeftOnWorker);
    EXPECT(StopRouteOf(true, false, false, true, false, true) == Route::Game);  // no list: the game's
    EXPECT(StopRouteOf(false, false, true, false, false, true) == Route::GameAndCopy);
    EXPECT(StopRouteOf(true, true, true, true, true, true) == Route::Two);
    EXPECT(StopRouteOf(false, true, true, true, true, false) == Route::Two);  // player 2 the primary player
    EXPECT(StopRouteOf(false, true, true, true, false, true) == Route::Game);  // not stopped there: never copied
    EXPECT(StopRouteOf(false, true, true, false, false, false) == Route::Game);
}

void TestListTick()
{
    std::puts("list tick: no list means the game's; player 2's component ticks its own; the game's never plays on pad 2");
    EXPECT(ListTickChoice(false, true, true) == ListTick::Game);
    EXPECT(ListTickChoice(true, true, false) == ListTick::Two);
    EXPECT(ListTickChoice(true, true, true) == ListTick::Two);
    EXPECT(ListTickChoice(true, false, true) == ListTick::Skip);
    EXPECT(ListTickChoice(true, false, false) == ListTick::GameOne);
}

}  // namespace

int main()
{
    TestKeySet();
    TestSeedsLoop();
    TestRoutes();
    TestListTick();

    return lss::test::Report();
}
