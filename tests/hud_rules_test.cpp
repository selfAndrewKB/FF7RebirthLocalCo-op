// Host-side tests for hud::hud_rules, run without the game.
//
// The placement streak and restore verdict, the compass and pin owners, the arrow-timer swap, the map piece's unload
// verdict, the gauge entries each pass hides, the enemy gauge classification, and the shared slot's rule across the
// overlay render's passes (hud/passes/overlay_pass.h).

#include "hud/hud_rules.h"
#include "hud/passes/overlay_pass.h"

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
    std::puts("pins: a new compass is every player's; whoever is active owns it; closing keeps the owners; pin owners");
    EXPECT(CompassOwners(false, 0, 1) == kEveryPlayer);
    EXPECT(CompassOwners(true, 1, kEveryPlayer) == 1);
    EXPECT(CompassOwners(true, 2, 1) == 2);
    EXPECT(CompassOwners(true, 3, 1) == 3);
    EXPECT(CompassOwners(true, 4, 1) == 4);  // player 3 alone
    EXPECT(CompassOwners(true, 0, 2) == 2);  // closing: nobody active, the owners kept
    EXPECT((kEveryPlayer & 0b111) == 0b111);  // player 1, 2 and 3 each see a new one
    EXPECT(PinOwnerOf(1) == 0);
    EXPECT(PinOwnerOf(2) == 1);
    EXPECT(PinOwnerOf(4) == 2);
    EXPECT(PinOwnerOf(3) == 0);  // player 1 first
    EXPECT(PinOwnerOf(6) == 1);  // then the lower seat
    EXPECT(PinOwnerOf(0) == -1);
}

void TestTimerSwap()
{
    std::puts("timers: an unseen arrow starts at 0 for player 2; player 1's come back; a new element keeps its value; "
              "gone keys are dropped");
    TimerSwap swap;
    swap.BeginIn();
    EXPECT(swap.In(1, 5.0f) == 0.0f);  // never seen by the seat's run
    EXPECT(swap.In(2, 6.0f) == 0.0f);
    swap.BeginOut();
    EXPECT(swap.Out(1, 0.5f) == std::optional<float>(5.0f));
    EXPECT(swap.Out(2, 0.7f) == std::optional<float>(6.0f));
    EXPECT(!swap.Out(3, 0.9f).has_value());  // new in the seat's run: keeps what it has
    swap.EndOut();
    EXPECT(swap.SeatTimerCount() == 3);

    swap.BeginIn();
    EXPECT(swap.In(1, 5.1f) == 0.5f);  // the seat's own timer
    EXPECT(swap.In(3, 1.0f) == 0.9f);
    EXPECT(swap.In(4, 2.0f) == 0.0f);
    swap.BeginOut();
    EXPECT(swap.Out(1, 0.6f) == std::optional<float>(5.1f));
    EXPECT(swap.Out(4, 0.1f) == std::optional<float>(2.0f));
    swap.EndOut();  // key 2 and 3 are gone from the list: dropped
    EXPECT(swap.SeatTimerCount() == 2);
    swap.BeginIn();
    EXPECT(swap.In(3, 1.0f) == 0.0f);
    swap.Abandon();
    swap.BeginOut();
    EXPECT(!swap.Out(3, 0.2f).has_value());  // abandoned: player 1's were left in place, nothing to give back
    swap.EndOut();
    swap.Forget();
    EXPECT(swap.SeatTimerCount() == 0);
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

    // Two players: player 1's target only is out of player 2's view, player 2's only out of player 1's.
    EXPECT(!GaugeHiddenIn(1, 0) && GaugeHiddenIn(1, 1));
    EXPECT(GaugeHiddenIn(2, 0) && !GaugeHiddenIn(2, 1));
    EXPECT(!GaugeHiddenIn(3, 0) && !GaugeHiddenIn(3, 1));  // both target it
    EXPECT(!GaugeHiddenIn(0, 0) && !GaugeHiddenIn(0, 1));  // nobody's: the game's
    // Three players: each view keeps its own player's targets, whoever else targets them too.
    EXPECT(GaugeHiddenIn(4, 0) && GaugeHiddenIn(4, 1) && !GaugeHiddenIn(4, 2));
    EXPECT(!GaugeHiddenIn(5, 0) && GaugeHiddenIn(5, 1) && !GaugeHiddenIn(5, 2));
    EXPECT(!GaugeHiddenIn(7, 0) && !GaugeHiddenIn(7, 1) && !GaugeHiddenIn(7, 2));
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
    std::puts("shared slot: the first of players 2 and 3's passes holds its acquire, later ones take it, player 1's takes it back");
    using lss::hud::overlay_pass::AcquireAction;
    using lss::hud::overlay_pass::IsSeatEntry;
    using lss::hud::overlay_pass::SlotAction;
    // A seat's pass (seat 2, then seat 1): the first acquire of an active slot is held; a later seat's pass takes the
    // held slot; a second acquire in one pass, or a slot that takes no part (the minimap not painting for the seat), is
    // the game's own.
    EXPECT(AcquireAction(2, true, false, false) == SlotAction::AcquireAndHold);
    EXPECT(AcquireAction(1, true, true, false) == SlotAction::TakeHeld);
    EXPECT(AcquireAction(1, true, false, false) == SlotAction::AcquireAndHold);
    EXPECT(AcquireAction(1, true, true, true) == SlotAction::Acquire);
    EXPECT(AcquireAction(1, false, false, false) == SlotAction::Acquire);
    EXPECT(AcquireAction(1, false, true, false) == SlotAction::Acquire);
    // Player 1's pass: the held slot comes back; nothing held (no seat's pass ran) is a plain acquire.
    EXPECT(AcquireAction(0, true, true, false) == SlotAction::TakeBack);
    EXPECT(AcquireAction(0, false, true, true) == SlotAction::TakeBack);
    EXPECT(AcquireAction(0, true, false, false) == SlotAction::Acquire);
    // Outside the render: always the game's own, even with a slot left held.
    EXPECT(AcquireAction(lss::hud::overlay_pass::kNone, true, true, false) == SlotAction::Acquire);
    EXPECT(AcquireAction(lss::hud::overlay_pass::kNone, true, false, false) == SlotAction::Acquire);
    // The entry added to the held slot in a seat's pass is the seat's, its submit left to player 1's pass; any other
    // slot or pass is the game's.
    EXPECT(IsSeatEntry(1, true));
    EXPECT(IsSeatEntry(2, true));
    EXPECT(!IsSeatEntry(1, false));
    EXPECT(!IsSeatEntry(0, true));
    EXPECT(!IsSeatEntry(lss::hud::overlay_pass::kNone, true));
    // The pass state itself: one setter, one reader; anything negative is outside the render.
    lss::hud::overlay_pass::Set(2);
    EXPECT(lss::hud::overlay_pass::Seat() == 2 && lss::hud::overlay_pass::IsSeatPass());
    lss::hud::overlay_pass::Set(0);
    EXPECT(lss::hud::overlay_pass::Seat() == 0 && !lss::hud::overlay_pass::IsSeatPass());
    lss::hud::overlay_pass::Set(-5);
    EXPECT(lss::hud::overlay_pass::Seat() == lss::hud::overlay_pass::kNone && !lss::hud::overlay_pass::IsSeatPass());
}

void TestPassKinds()
{
    std::puts("passes: a seat's own view gets its own pass; its column on player 1's picture gets one of player 1's HUD "
              "only on a monitor of another shape");
    using lss::hud::overlay_pass::OtherShape;
    using lss::hud::overlay_pass::PassKindOf;
    using lss::hud::overlay_pass::SeatPassKind;
    constexpr float kSixteenNine = 2560.0f / 1440.0f;
    constexpr float kUltrawide   = 3440.0f / 1440.0f;
    constexpr float kHalf        = 960.0f / 1080.0f;
    // The shape test: 1 % apart either way; unknown shapes never differ.
    EXPECT(!OtherShape(kSixteenNine, 1920.0f / 1080.0f));
    EXPECT(OtherShape(kUltrawide, kSixteenNine) && OtherShape(kSixteenNine, kUltrawide));
    EXPECT(!OtherShape(kSixteenNine * 1.005f, kSixteenNine) && OtherShape(kSixteenNine * 1.02f, kSixteenNine));
    EXPECT(!OtherShape(0.0f, kSixteenNine) && !OtherShape(kUltrawide, 0.0f));
    // Field play: the seat's own view, whatever its shape.
    EXPECT(PassKindOf(true, true, false, true, kUltrawide, kSixteenNine) == SeatPassKind::Own);
    EXPECT(PassKindOf(true, true, false, true, kHalf, kHalf) == SeatPassKind::Own);
    // Out of the game: none.
    EXPECT(PassKindOf(false, true, false, true, kHalf, kHalf) == SeatPassKind::None);
    // A menu on a row: the seat's column on player 1's picture; the wide monitor gets player 1's HUD laid out for it,
    // a monitor of player 1's shape samples player 1's image.
    EXPECT(PassKindOf(true, false, true, true, kUltrawide, kSixteenNine) == SeatPassKind::PlayerOnes);
    EXPECT(PassKindOf(true, false, true, true, 1920.0f / 1080.0f, kSixteenNine) == SeatPassKind::None);
    // A lone waiting player beside another's own view: the waiting one on player 1's picture.
    EXPECT(PassKindOf(true, true, true, true, kUltrawide, kSixteenNine) == SeatPassKind::PlayerOnes);
    EXPECT(PassKindOf(true, true, true, true, kSixteenNine, kSixteenNine) == SeatPassKind::None);
    // The engine's single view (one window): no column, no pass; the shape not yet known: none.
    EXPECT(PassKindOf(true, false, true, false, kUltrawide, kSixteenNine) == SeatPassKind::None);
    EXPECT(PassKindOf(true, false, true, true, 0.0f, kSixteenNine) == SeatPassKind::None);
}

int main()
{
    TestPlacement();
    TestPinOwners();
    TestTimerSwap();
    TestUnloadAndGauges();
    TestGaugeClass();
    TestSharedSlot();
    TestPassKinds();

    return lss::test::Report();
}
