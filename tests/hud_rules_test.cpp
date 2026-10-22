// Host-side tests for hud::hud_rules, run without the game.
//
// The placement streak and restore verdict, the compass and pin owners, the arrow-timer swap, the map piece's unload
// verdict, the gauge entries each pass hides, and the enemy gauge classification.

#include "hud/hud_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::hud::hud_rules;

void TestPlacement()
{
    std::puts("placement: a gap of at most two ticks extends the streak, a longer one resets it; the restore verdict");
    EXPECT(StreakAfter(10, 9, 3) == 4);
    EXPECT(StreakAfter(10, 8, 3) == 4);
    EXPECT(StreakAfter(10, 7, 3) == 0);
    EXPECT(StreakAfter(10, 10, 0) == 1);

    int reads = 0;
    const auto applied = [&] { ++reads; return true; };
    const auto changed = [&] { ++reads; return false; };
    EXPECT(RestoreVerdict(false, false, true, applied) == Restore::Skip);
    EXPECT(RestoreVerdict(true, true, true, applied) == Restore::Skip);
    EXPECT(RestoreVerdict(true, false, false, applied) == Restore::Skip);
    EXPECT(reads == 0);
    EXPECT(RestoreVerdict(true, false, true, applied) == Restore::Restore);
    EXPECT(RestoreVerdict(true, false, true, changed) == Restore::GamesOwn);
    EXPECT(reads == 2);
}

void TestPinOwners()
{
    std::puts("pins: a new compass is both players'; whoever is active owns it; closing keeps the owners; pin owners");
    EXPECT(CompassOwners(false, false, false, 1) == 3);
    EXPECT(CompassOwners(true, true, false, 3) == 1);
    EXPECT(CompassOwners(true, false, true, 1) == 2);
    EXPECT(CompassOwners(true, true, true, 1) == 3);
    EXPECT(CompassOwners(true, false, false, 2) == 2);  // closing: nobody active, the owners kept
    EXPECT(PinOwnerOf(true, false) == 0);
    EXPECT(PinOwnerOf(false, true) == 1);
    EXPECT(PinOwnerOf(true, true) == 0);  // player 1 first
    EXPECT(PinOwnerOf(false, false) == -1);
}

void TestTimerSwap()
{
    std::puts("timers: an unseen arrow starts at 0 for player 2; player 1's come back; a new element keeps its value; "
              "gone keys are dropped");
    TimerSwap swap;
    swap.BeginIn();
    EXPECT(swap.In(1, 5.0f) == 0.0f);  // never seen by player 2's run
    EXPECT(swap.In(2, 6.0f) == 0.0f);
    swap.BeginOut();
    EXPECT(swap.Out(1, 0.5f) == std::optional<float>(5.0f));
    EXPECT(swap.Out(2, 0.7f) == std::optional<float>(6.0f));
    EXPECT(!swap.Out(3, 0.9f).has_value());  // new in player 2's run: keeps what it has
    swap.EndOut();
    EXPECT(swap.TwoCount() == 3);

    swap.BeginIn();
    EXPECT(swap.In(1, 5.1f) == 0.5f);  // player 2's own timer
    EXPECT(swap.In(3, 1.0f) == 0.9f);
    EXPECT(swap.In(4, 2.0f) == 0.0f);
    swap.BeginOut();
    EXPECT(swap.Out(1, 0.6f) == std::optional<float>(5.1f));
    EXPECT(swap.Out(4, 0.1f) == std::optional<float>(2.0f));
    swap.EndOut();  // key 2 and 3 are gone from the list: dropped
    EXPECT(swap.TwoCount() == 2);
    swap.BeginIn();
    EXPECT(swap.In(3, 1.0f) == 0.0f);
    swap.Abandon();
    swap.BeginOut();
    EXPECT(!swap.Out(3, 0.2f).has_value());  // abandoned: player 1's were left in place, nothing to give back
    swap.EndOut();
    swap.Forget();
    EXPECT(swap.TwoCount() == 0);
}

void TestUnloadAndGauges()
{
    std::puts("unload: the game's answer unless it would drop a piece the other player's map shows; the gauge passes");
    int asked = 0;
    const auto otherDrops = [&] { ++asked; return true; };
    const auto otherKeeps = [&] { ++asked; return false; };
    EXPECT(UnloadVerdict(false, true, otherDrops) == Unload::GamesOwn);
    EXPECT(UnloadVerdict(true, false, otherDrops) == Unload::GamesOwn);
    EXPECT(asked == 0);
    EXPECT(UnloadVerdict(true, true, otherKeeps) == Unload::KeptForOther);
    EXPECT(UnloadVerdict(true, true, otherDrops) == Unload::Dropped);
    EXPECT(asked == 2);

    EXPECT(OtherGaugePass(true, false) == 1);
    EXPECT(OtherGaugePass(false, true) == 0);
    EXPECT(OtherGaugePass(true, true) == -1);
    EXPECT(OtherGaugePass(false, false) == -1);
}

void TestGaugeClass()
{
    std::puts("enemy gauges: own (two steps, leader scope), main, case C (no leader scope), forced; the player-1 rule");
    auto gauge = ClassifyGauge(0x10, 0x10, 0x20, false, false, false);
    EXPECT(gauge.own && !gauge.main && gauge.target && gauge.steps == 2 && gauge.leaderScope && !gauge.caseC);
    gauge = ClassifyGauge(0x20, 0x10, 0x20, false, false, false);
    EXPECT(!gauge.own && gauge.main && gauge.target && gauge.steps == 1 && gauge.leaderScope);
    gauge = ClassifyGauge(0x30, 0x10, 0x20, true, true, false);
    EXPECT(gauge.caseC && gauge.target && !gauge.leaderScope && gauge.steps == 1);
    gauge = ClassifyGauge(0x30, 0x10, 0x20, true, false, false);
    EXPECT(!gauge.target && !gauge.leaderScope);
    gauge = ClassifyGauge(0x30, 0x10, 0x20, false, false, true);
    EXPECT(gauge.target && gauge.leaderScope);
    gauge = ClassifyGauge(0, 0, 0, false, false, false);  // a null source never matches a null target
    EXPECT(!gauge.own && !gauge.main && !gauge.target);

    EXPECT(PlayerOneTargetRule(0x10, 0x10, 0x20, false, false, false));
    EXPECT(PlayerOneTargetRule(0x20, 0x10, 0x20, false, false, false));
    EXPECT(!PlayerOneTargetRule(0x30, 0x10, 0x20, false, false, false));
    EXPECT(PlayerOneTargetRule(0x30, 0x10, 0x20, true, true, false));
    EXPECT(!PlayerOneTargetRule(0x30, 0x10, 0x20, true, false, false));
    EXPECT(PlayerOneTargetRule(0x30, 0x10, 0x20, false, false, true));
    EXPECT(!PlayerOneTargetRule(0, 0, 0, false, false, false));
}

}  // namespace

void TestSharedSlot()
{
    std::puts("shared slot: player 2's pass holds its acquire, player 1's takes it back; the held entry is player 2's");
    using lss::hud::OverlayPass;
    // Player 2's pass: the first acquire of an active slot is held; a second acquire in the same pass, or a slot that
    // takes no part (the minimap not painting for player 2), is the game's own.
    EXPECT(SlotActionFor(OverlayPass::PlayerTwo, true, false) == SlotAction::AcquireAndHold);
    EXPECT(SlotActionFor(OverlayPass::PlayerTwo, true, true) == SlotAction::Acquire);
    EXPECT(SlotActionFor(OverlayPass::PlayerTwo, false, false) == SlotAction::Acquire);
    // Player 1's pass: the held slot comes back; nothing held (player 2's pass did not run) is a plain acquire.
    EXPECT(SlotActionFor(OverlayPass::PlayerOne, true, true) == SlotAction::TakeBack);
    EXPECT(SlotActionFor(OverlayPass::PlayerOne, false, true) == SlotAction::TakeBack);
    EXPECT(SlotActionFor(OverlayPass::PlayerOne, true, false) == SlotAction::Acquire);
    // Outside the render: always the game's own, even with a slot left held.
    EXPECT(SlotActionFor(OverlayPass::None, true, true) == SlotAction::Acquire);
    EXPECT(SlotActionFor(OverlayPass::None, true, false) == SlotAction::Acquire);
    // The entry added to the held slot in player 2's pass is named for player 2 and its submit left to player 1's
    // pass; any other slot or pass is the game's.
    EXPECT(PlayerTwoEntry(OverlayPass::PlayerTwo, true));
    EXPECT(!PlayerTwoEntry(OverlayPass::PlayerTwo, false));
    EXPECT(!PlayerTwoEntry(OverlayPass::PlayerOne, true));
    EXPECT(!PlayerTwoEntry(OverlayPass::None, true));
    // The pass state itself: one setter, one reader.
    lss::hud::SetOverlayPass(OverlayPass::PlayerTwo);
    EXPECT(lss::hud::CurrentOverlayPass() == OverlayPass::PlayerTwo);
    lss::hud::SetOverlayPass(OverlayPass::None);
    EXPECT(lss::hud::CurrentOverlayPass() == OverlayPass::None);
}

int main()
{
    TestPlacement();
    TestPinOwners();
    TestTimerSwap();
    TestUnloadAndGauges();
    TestGaugeClass();
    TestSharedSlot();

    return lss::test::Report();
}
