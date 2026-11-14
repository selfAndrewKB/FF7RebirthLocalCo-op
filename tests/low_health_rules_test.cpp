// Host-side tests for the low-health red edge's rules (hud/low_health_rules.h), run without the game.

#include "hud/low_health_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::hud::low_health_rules;

void TestCounts()
{
    std::puts("counts: a seat its own member only; player 1 every member no seat plays");
    const Held held{kNoType, 3, kNoType};  // player 2 plays type 3, player 3 nothing
    EXPECT(Counts(1, 3, held));
    EXPECT(!Counts(1, 0, held));
    EXPECT(!Counts(2, 3, held));
    EXPECT(!Counts(2, kNoType, held));
    EXPECT(Counts(0, 0, held));
    EXPECT(Counts(0, 5, held));
    EXPECT(!Counts(0, 3, held));
    const Held none{kNoType, kNoType, kNoType};
    EXPECT(Counts(0, 3, none));  // one player: every member is player 1's to count
    EXPECT(!Counts(3, 3, held));  // past the players
}

void TestTargetAndView()
{
    std::puts("target: on while a filter shows and the edge is allowed; a view: its own fade capped by the game's");
    EXPECT(Target(1, true, 1.0f) == 1.0f);
    EXPECT(Target(4, true, 1.0f) == 1.0f);
    EXPECT(Target(1, false, 1.0f) == 0.0f);
    EXPECT(Target(0, true, 1.0f) == 0.0f);
    EXPECT(ViewIntensity(0.3f, 0.8f) == 0.3f);
    EXPECT(ViewIntensity(1.0f, 0.4f) == 0.4f);  // a menu's fade-out reaches every view
    EXPECT(ViewIntensity(0.0f, 1.0f) == 0.0f);
}

void TestViewPlayer()
{
    std::puts("view player: player 1's id and a mirror's are player 1's, a seat's id its seat's, else none");
    const std::array<std::int32_t, kPlayers> seats{-2, 1, 2};
    EXPECT(ViewPlayer(0, 0, seats) == 0);
    EXPECT(ViewPlayer(1, 0, seats) == 1);
    EXPECT(ViewPlayer(2, 0, seats) == 2);
    EXPECT(ViewPlayer(lss::players::kFirstMirrorId, 0, seats) == 0);
    EXPECT(ViewPlayer(-1, 0, seats) == -1);  // a scene capture
    EXPECT(ViewPlayer(3, 0, seats) == -1);
    const std::array<std::int32_t, kPlayers> out{-2, -2, -2};
    EXPECT(ViewPlayer(1, 0, out) == -1);  // a seat out: its id is no view of a player's
}

void TestHolds()
{
    std::puts("holds: a count taken once per filter; the Dying off gives back the Stone's too");
    HoldStep step = OnEnable(0, kHoldDying);
    EXPECT(step.holds == kHoldDying && step.take == kHoldDying && step.give == 0);
    step = OnEnable(step.holds, kHoldDying);
    EXPECT(step.holds == kHoldDying && step.take == 0);
    step = OnEnable(step.holds, kHoldStone);
    EXPECT(step.holds == (kHoldDying | kHoldStone) && step.take == kHoldStone);
    step = OnDisable(step.holds, false);
    EXPECT(step.holds == kHoldDying && step.give == kHoldStone);
    step = OnDisable(kHoldDying | kHoldStone, true);
    EXPECT(step.holds == 0 && step.give == (kHoldDying | kHoldStone));
    step = OnDisable(0, true);
    EXPECT(step.holds == 0 && step.give == 0);  // nothing held: nothing given back
}

void TestHoldsFor()
{
    std::puts("holds for a danger value: Dying's count, Stone's count, none; a hold beyond them goes back");
    EXPECT(HoldsFor(1) == kHoldDying);
    EXPECT(HoldsFor(4) == kHoldStone);
    EXPECT(HoldsFor(0) == 0);
    const std::uint8_t held = kHoldDying;
    EXPECT((held & ~HoldsFor(0)) == kHoldDying);  // the game's reset zeroed the value: the Dying count goes back
    EXPECT((held & ~HoldsFor(1)) == 0);
}

}  // namespace

int main()
{
    TestCounts();
    TestTargetAndView();
    TestViewPlayer();
    TestHolds();
    TestHoldsFor();
    return lss::test::Report();
}
