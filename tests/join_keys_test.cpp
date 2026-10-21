// Host-side tests for players::ResolveIntent, run without the game.
//
// Joining and leaving win over everything else pressed on the same tick, and cancel each other; of the switches, one
// per tick, in the keys' priority.

#include "players/join_keys.h"

#include "expect.h"

#include <cstdio>

namespace {

using lss::players::PlayerIntent;
using lss::players::ResolveIntent;

void TestIntents()
{
    std::puts("intents: join and leave first and cancelling; one switch per tick in priority order");
    EXPECT(ResolveIntent(false, false, false, false, false, false, false, false, false, false) == PlayerIntent::None);
    EXPECT(ResolveIntent(true, false, true, true, true, true, true, true, true, true) == PlayerIntent::Join);
    EXPECT(ResolveIntent(false, true, true, true, true, true, true, true, true, true) == PlayerIntent::Leave);
    EXPECT(ResolveIntent(true, true, true, false, false, false, false, false, false, false) == PlayerIntent::None);
    EXPECT(ResolveIntent(false, false, true, true, true, true, true, true, true, true) == PlayerIntent::ToggleOwnCamera);
    EXPECT(ResolveIntent(false, false, false, true, true, true, true, true, true, true) == PlayerIntent::ToggleLayout);
    EXPECT(ResolveIntent(false, false, false, false, true, true, true, true, true, true) == PlayerIntent::ToggleFill);
    EXPECT(ResolveIntent(false, false, false, false, false, true, true, true, true, true) == PlayerIntent::TogglePadSticks);
    EXPECT(ResolveIntent(false, false, false, false, false, false, true, true, true, true) == PlayerIntent::ToggleNpcCentre);
    EXPECT(ResolveIntent(false, false, false, false, false, false, false, true, true, true) == PlayerIntent::ToggleNpcDetail);
    EXPECT(ResolveIntent(false, false, false, false, false, false, false, false, true, true) == PlayerIntent::ToggleThirdView);
    EXPECT(ResolveIntent(false, false, false, false, false, false, false, false, false, true) == PlayerIntent::ToggleWindowSpan);
}

}  // namespace

int main()
{
    TestIntents();

    return lss::test::Report();
}
