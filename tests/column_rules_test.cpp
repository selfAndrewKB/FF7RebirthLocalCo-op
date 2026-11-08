// Host-side tests for the engine's layout records (views/column_rules.h): the columns of their monitors' widths, the
// stock entries and the decisions over the live entry. Taken from Remake's tests/column_rules_test.cpp (the rows of
// the records and the decisions); Rebirth's rows: the stock three-column entry, equal widths as the stock records.

#include "views/column_rules.h"

#include "expect.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

namespace {

using namespace lss::views::column_rules;

/// The pixels the engine makes of a fraction: TruncToInt of the float product.
std::int32_t Pixels(float fraction, std::int32_t total)
{
    return static_cast<std::int32_t>(std::trunc(fraction * static_cast<float>(total)));
}

/// The stock entries: three equal full-height columns tiling the width exactly; the two-player ones distinct.
void StockEntries()
{
    std::puts("column rules: the stock three-column entry tiles the window; the two-player entries are distinct");
    float width = 0;
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        EXPECT(kStockThreeColumns[i].sizeY == 1.0f && kStockThreeColumns[i].originY == 0.0f);
        EXPECT(kStockThreeColumns[i].originX == width);
        width += kStockThreeColumns[i].sizeX;
    }
    EXPECT(width > 0.999f && width < 1.001f);
    EXPECT(Same(kStockThreeColumns, kStockThreeColumns));
    EXPECT(!Same(kStockTopBottom, kStockSideBySide));
    EXPECT(kStockSideBySide[1].originX == 0.5f && kStockTopBottom[1].originY == 0.5f);
}

/// Columns of their own widths (the span's monitors): each record's fractions give back the monitor's exact pixels
/// through the engine's float product and truncation, the columns touch, and equal widths are the stock columns.
void ColumnsOfTheirWidths()
{
    std::puts("column rules: each column lands on its monitor's edges through the engine's truncation");
    const std::array<std::int32_t, 3> row{2560, 3440, 2560};  // a row at 1440, the ultrawide in the middle
    const Records                     columns = ColumnRecords(row);
    std::int32_t                      left    = 0;
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT(Pixels(columns.record[i].originX, 8560) == left);
        EXPECT(Pixels(columns.record[i].sizeX, 8560) == row[i]);
        EXPECT(columns.record[i].sizeY == 1.0f && columns.record[i].originY == 0.0f);
        left += row[i];
    }
    // Every width pair and triple of common monitors lands on its edges.
    const std::int32_t widths[] = {1280, 1366, 1600, 1920, 2560, 2880, 3440, 3840, 5120};
    for (const std::int32_t a : widths)
    {
        for (const std::int32_t b : widths)
        {
            for (const std::int32_t c : widths)
            {
                const std::array<std::int32_t, 3> three{a, b, c};
                const Records                     r     = ColumnRecords(three);
                const std::int32_t                total = a + b + c;
                EXPECT(Pixels(r.record[1].originX, total) == a && Pixels(r.record[2].originX, total) == a + b);
                EXPECT(Pixels(r.record[0].sizeX, total) == a && Pixels(r.record[1].sizeX, total) == b &&
                       Pixels(r.record[2].sizeX, total) == c);
            }
            const TwoRecords two = ColumnRecords(std::array<std::int32_t, 2>{a, b});
            EXPECT(Pixels(two.record[1].originX, a + b) == a && Pixels(two.record[0].sizeX, a + b) == a &&
                   Pixels(two.record[1].sizeX, a + b) == b);
        }
    }
    // Equal widths: the stock records themselves (the nearest floats to the thirds), so a row of equal monitors
    // writes nothing.
    EXPECT(Same(ColumnRecords(std::array<std::int32_t, 3>{1920, 1920, 1920}).record, kStockThreeColumns));
    EXPECT(Same(ColumnRecords(std::array<std::int32_t, 3>{2560, 2560, 2560}).record, kStockThreeColumns));
    EXPECT(Same(ColumnRecords(std::array<std::int32_t, 2>{2560, 2560}).record, kStockSideBySide));
    // Bad widths: every record empty.
    const Records bad = ColumnRecords(std::array<std::int32_t, 3>{2560, 0, 2560});
    EXPECT(bad.record[0].sizeX == 0.0f && bad.record[1].sizeX == 0.0f && bad.record[2].sizeX == 0.0f);
    EXPECT(EdgeFraction(0, 100) == 0.0f && EdgeFraction(5, 0) == 0.0f && EdgeFraction(200, 100) == 0.0f);
}

/// Heights: a monitor shorter than the row's tallest gets a view of its own height at the window's top.
void ColumnsOfTheirHeights()
{
    std::puts("column rules: a shorter monitor's view is as tall as the monitor, at the top");
    const Records rows = ColumnRecords(std::array<std::int32_t, 3>{2560, 2560, 1280}, std::array<std::int32_t, 3>{1440, 1440, 720});
    EXPECT(rows.record[0].sizeY == 1.0f && rows.record[1].sizeY == 1.0f);
    EXPECT(Pixels(rows.record[2].sizeY, 1440) == 720 && rows.record[2].originY == 0.0f);
    EXPECT(Pixels(rows.record[2].originX, 6400) == 5120 && Pixels(rows.record[2].sizeX, 6400) == 1280);
    // One height: the widths' records as they are.
    EXPECT(Same(ColumnRecords(std::array<std::int32_t, 2>{3440, 2560}, std::array<std::int32_t, 2>{1440, 1440}).record,
                ColumnRecords(std::array<std::int32_t, 2>{3440, 2560}).record));
    // A bad height: the widths' records, every view full height.
    EXPECT(Same(ColumnRecords(std::array<std::int32_t, 3>{2560, 3440, 2560}, std::array<std::int32_t, 3>{1440, 0, 1440}).record,
                ColumnRecords(std::array<std::int32_t, 3>{2560, 3440, 2560}).record));
}

/// What the module does with the entry it read, given the entry wanted.
void Decisions()
{
    std::puts("column rules: the stock entry written over, the mod's restored, anything else left alone");
    const Records columns = ColumnRecords(std::array<std::int32_t, 3>{2560, 3440, 2560});
    const Records other   = ColumnRecords(std::array<std::int32_t, 3>{1920, 2560, 1920});
    const Records kRight  = ThreeSplitRecords(ThreeSplit::Right);
    EXPECT(Decide(kStockThreeColumns, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Write);
    EXPECT(Decide(kStockThreeColumns, kStockThreeColumns, columns.record, kRight.record, kStockThreeColumns) == Action::Nothing);
    EXPECT(Decide(columns.record, columns.record, columns.record, kRight.record, columns.record) == Action::Nothing);
    EXPECT(Decide(columns.record, kStockThreeColumns, columns.record, kRight.record, columns.record) == Action::Restore);
    EXPECT(Decide(columns.record, other.record, other.record, kRight.record, columns.record) == Action::Write);    // the row changed
    EXPECT(Decide(columns.record, other.record, other.record, kRight.record, kStockThreeColumns) == Action::Foreign);  // never written
    Record foreign[kThreePlayerRecords] = {{0.5f, 1.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT(Decide(foreign, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    EXPECT(Decide(foreign, kStockThreeColumns, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    Record zero[kThreePlayerRecords] = {};
    EXPECT(Decide(zero, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    // Partly the mod's (two records written, the third stock): foreign, never "restored" over.
    Record partly[kThreePlayerRecords] = {columns.record[0], columns.record[1], kStockThreeColumns[2]};
    EXPECT(Decide(partly, kStockThreeColumns, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    EXPECT(Decide(partly, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    // A record twice is no arrangement: foreign.
    Record twice[kThreePlayerRecords] = {columns.record[0], columns.record[1], columns.record[1]};
    EXPECT(Decide(twice, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    // The one-window top layout's records while the layout in effect is another: foreign; while it is top: the mod's.
    Record favorTop[kThreePlayerRecords] = {{1.0f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};
    EXPECT(Decide(favorTop, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    EXPECT(Decide(favorTop, columns.record, columns.record, ThreeSplitRecords(ThreeSplit::Top).record, kStockThreeColumns) ==
           Action::Write);
    // A NaN never equals anything: foreign, left alone.
    Record nan[kThreePlayerRecords] = {kStockThreeColumns[0], kStockThreeColumns[1],
                                       {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, std::numeric_limits<float>::quiet_NaN()}};
    EXPECT(Decide(nan, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Foreign);
    // Negative zero equals zero: still the stock entry.
    Record negativeZero[kThreePlayerRecords] = {{1.0f / 3.0f, 1.0f, -0.0f, -0.0f}, kStockThreeColumns[1], kStockThreeColumns[2]};
    EXPECT(Decide(negativeZero, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Write);
    // The stock records in another order (an arrangement): one of the known entries, restored to the stock order.
    Record swapped[kThreePlayerRecords] = {kStockThreeColumns[0], kStockThreeColumns[2], kStockThreeColumns[1]};
    EXPECT(Decide(swapped, kStockThreeColumns, columns.record, kRight.record, kStockThreeColumns) == Action::Restore);
    EXPECT(Decide(swapped, columns.record, columns.record, kRight.record, kStockThreeColumns) == Action::Write);

    // Two players on a two-monitor span: the side-by-side entry from the two widths.
    const TwoRecords side = ColumnRecords(std::array<std::int32_t, 2>{3440, 2560});
    EXPECT(DecideTwo(kStockSideBySide, side.record, kStockSideBySide, side.record, kStockSideBySide) == Action::Write);
    EXPECT(DecideTwo(side.record, kStockSideBySide, kStockSideBySide, side.record, side.record) == Action::Restore);
    EXPECT(DecideTwo(side.record, side.record, kStockSideBySide, kStockSideBySide, side.record) == Action::Nothing);
    EXPECT(DecideTwo(side.record, kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Foreign);
    EXPECT(DecideTwo(kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Nothing);
    EXPECT(DecideTwo(kStockTopBottom, side.record, kStockSideBySide, side.record, kStockSideBySide) == Action::Foreign);
}

/// The entries wanted: the span's columns or the one-window layout, arranged, while three are listed; the two-player
/// entries arranged while two are; the stock records otherwise.
void Wanted()
{
    std::puts("column rules: the entries wanted while the players are listed, the stock ones otherwise");
    const Records columns = ColumnRecords(std::array<std::int32_t, 3>{2560, 3440, 2560});
    const Records top     = ThreeSplitRecords(ThreeSplit::Top);
    EXPECT(Same(ThreeWanted(true, columns.record, top.record, 3, kDefaultOrder, {0, 1, 2}).record, columns.record));
    EXPECT(Same(ThreeWanted(false, columns.record, top.record, 3, kDefaultOrder, {0, 1, 2}).record, top.record));
    EXPECT(Same(ThreeWanted(true, columns.record, top.record, 2, kDefaultOrder, {0, 1, -1}).record, kStockThreeColumns));
    EXPECT(Same(ThreeWanted(false, columns.record, top.record, 1, kDefaultOrder, {0, -1, -1}).record, kStockThreeColumns));
    const TwoRecords side = ColumnRecords(std::array<std::int32_t, 2>{3440, 2560});
    EXPECT(Same(TwoWanted(side.record, kStockSideBySide, 2, kDefaultOrder, {0, 1, -1}).record, side.record));
    EXPECT(Same(TwoWanted(side.record, kStockSideBySide, 3, kDefaultOrder, {0, 1, 2}).record, kStockSideBySide));
    EXPECT(Same(TwoWanted(side.record, kStockSideBySide, 1, kDefaultOrder, {0, -1, -1}).record, kStockSideBySide));
}

void Orders()
{
    std::puts("column rules: the order from its words, by player number");
    EXPECT(ParseOrder(L"1 2 3") == kDefaultOrder);
    EXPECT(ParseOrder(L"123") == kDefaultOrder);
    EXPECT((ParseOrder(L"1, 3, 2") == Order{{1, 3, 2}}));
    EXPECT((ParseOrder(L" 132 ") == Order{{1, 3, 2}}));
    EXPECT((ParseOrder(L"213") == Order{{2, 1, 3}}));  // player 1 may take any screen
    EXPECT((ParseOrder(L"3 2 1") == Order{{3, 2, 1}}));
    EXPECT(!ParseOrder(L"124"));
    EXPECT(!ParseOrder(L"012"));
    EXPECT(!ParseOrder(L"12"));
    EXPECT(!ParseOrder(L"1233"));
    EXPECT(!ParseOrder(L"122"));
    EXPECT(!ParseOrder(L""));
    EXPECT(!ParseOrder(L"one two"));
}

/// Each view's slot: a player's from the order, the mirrors' the ones left over in list order.
void SlotsAndEntries()
{
    std::puts("column rules: each view's slot by the order, whatever the joins' order");
    using Slots3 = std::array<int, kThreePlayerRecords>;
    const Order swap{{1, 3, 2}};
    EXPECT((Slots(kDefaultOrder, {0, 1, 2}) == Slots3{0, 1, 2}));
    EXPECT((Slots(kDefaultOrder, {0, 2, 1}) == Slots3{0, 2, 1}));  // player 3 joined first: still on the right
    EXPECT((Slots(swap, {0, 1, 2}) == Slots3{0, 2, 1}));
    EXPECT((Slots(swap, {0, 2, 1}) == Slots3{0, 1, 2}));
    EXPECT((Slots(swap, {0, 1, -1}) == Slots3{0, 2, 1}));  // player 2 and the mirror: the mirror in the middle
    EXPECT((Slots(kDefaultOrder, {0, 2, -1}) == Slots3{0, 2, 1}));
    EXPECT((Slots(kDefaultOrder, {0, -1, -1}) == Slots3{0, 1, 2}));  // two mirrors in list order
    const Records span = ThreeWanted(true, kStockThreeColumns, ThreeSplitRecords(ThreeSplit::Top).record, 3, swap, {0, 1, 2});
    EXPECT(span.record[0] == kStockThreeColumns[0] && span.record[1] == kStockThreeColumns[2] &&
           span.record[2] == kStockThreeColumns[1]);
    const Records late = ThreeWanted(true, kStockThreeColumns, ThreeSplitRecords(ThreeSplit::Top).record, 3, kDefaultOrder, {0, 2, 1});
    EXPECT(late.record[1] == kStockThreeColumns[2] && late.record[2] == kStockThreeColumns[1]);
}

/// A view moved by its player: the order that shows each screen's seat.
void MovedOrders()
{
    std::puts("column rules: the order a moved view shows");
    using Seats3 = std::array<int, kThreePlayerRecords>;
    const auto both = OrderShowing(Seats3{0, 1, 2});
    EXPECT(both && *both == kDefaultOrder);
    const auto swapped = OrderShowing(Seats3{0, 2, 1});
    EXPECT(swapped && (swapped->player == Seats3{1, 3, 2}));
    const auto mirror = OrderShowing(Seats3{0, 1, -1});
    EXPECT(mirror && (mirror->player == Seats3{1, 2, 3}));
    const auto moved = OrderShowing(Seats3{1, 0, 2});
    EXPECT(moved && (moved->player == Seats3{2, 1, 3}));
    const auto twoMoved = OrderShowing(Seats3{1, 0, -1});
    EXPECT(twoMoved && (twoMoved->player == Seats3{2, 1, 3}));
    EXPECT(!OrderShowing(Seats3{0, 1, 1}));
    EXPECT(!OrderShowing(Seats3{0, 3, 1}));
    using Slots3 = std::array<int, kThreePlayerRecords>;
    EXPECT((ScreenSlots(3, *both, {0, 2, 1}) == Slots3{0, 2, 1}));
    EXPECT((ScreenSlots(2, *both, {0, 2, 1}) == Slots3{0, 1, 2}));
    EXPECT((ScreenSlots(2, *twoMoved, {0, 1, -1}) == Slots3{1, 0, 2}));
    EXPECT((ScreenSlots(1, *twoMoved, {0, 1, -1}) == Slots3{0, 1, 2}));
}

/// Two views: the two players in the order's order (the one not in skipped).
void TwoViews()
{
    std::puts("column rules: two views in the order's order");
    using Slots2 = std::array<int, kTwoPlayerRecords>;
    const Order p2First{{2, 1, 3}};
    const Order p3First{{3, 2, 1}};
    EXPECT((TwoSlots(kDefaultOrder, {0, 1, -1}) == Slots2{0, 1}));
    EXPECT((TwoSlots(p2First, {0, 1, -1}) == Slots2{1, 0}));
    EXPECT((TwoSlots(p2First, {0, 2, -1}) == Slots2{0, 1}));
    EXPECT((TwoSlots(p3First, {0, 2, -1}) == Slots2{1, 0}));
    EXPECT((TwoSlots(p2First, {0, -1, -1}) == Slots2{1, 0}));  // player 1 beside a mirror, moved second: the second screen
    EXPECT((TwoSlots(kDefaultOrder, {0, -1, -1}) == Slots2{0, 1}));
    EXPECT((TwoSlots(kDefaultOrder, {-1, 0, -1}) == Slots2{1, 0}));  // the mirror listed first: player 1 still first
    EXPECT((TwoSlots(kDefaultOrder, {-1, -1, -1}) == Slots2{0, 1}));
    // A view moved with a mirror beside it lands where the move put it (MoveView's round trip).
    const auto movedOrder = OrderShowing({-1, 0, -1});
    EXPECT(movedOrder && (TwoSlots(*movedOrder, {0, -1, -1}) == Slots2{1, 0}));
    const TwoRecords swapped = TwoWanted(kStockSideBySide, kStockSideBySide, 2, p2First, {0, 1, -1});
    EXPECT((swapped.record[0] == Record{0.5f, 1.0f, 0.5f, 0.0f}) && (swapped.record[1] == Record{0.5f, 1.0f, 0.0f, 0.0f}));
    EXPECT(DecideTwo(kStockSideBySide, swapped.record, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Write);
    EXPECT(DecideTwo(swapped.record, kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Restore);
}

/// The one-window layouts: each covers the window exactly once, its large view first; the word and the layout name each
/// other; changing the layout moves the entry on, leaving three views restores the stock one.
void ThreeSplits()
{
    std::puts("column rules: the one-window layouts for three players");
    for (const wchar_t* word : {L"top", L"bottom", L"left", L"right"})
    {
        const auto split = ParseThreeSplit(word);
        EXPECT(split.has_value());
        EXPECT(std::wstring_view(ThreeSplitWord(*split)) == word);
        const Records records = ThreeSplitRecords(*split);
        float         area    = 0.0f;
        for (int i = 0; i < kThreePlayerRecords; ++i)
        {
            const Record& a = records.record[i];
            EXPECT(a.originX >= 0.0f && a.originY >= 0.0f && a.originX + a.sizeX <= 1.0001f && a.originY + a.sizeY <= 1.0001f);
            area += a.sizeX * a.sizeY;
            for (int j = i + 1; j < kThreePlayerRecords; ++j)
            {
                const Record& b = records.record[j];
                EXPECT(a.originX + a.sizeX <= b.originX + 0.0001f || b.originX + b.sizeX <= a.originX + 0.0001f ||
                       a.originY + a.sizeY <= b.originY + 0.0001f || b.originY + b.sizeY <= a.originY + 0.0001f);
            }
        }
        EXPECT(area > 0.999f && area < 1.001f);
        EXPECT(records.record[0].sizeX * records.record[0].sizeY >= records.record[1].sizeX * records.record[1].sizeY);
    }
    EXPECT(!ParseThreeSplit(L"middle").has_value());
    EXPECT(!ParseThreeSplit(L"columns").has_value());  // three equal columns are the span's only (the user, 2026-11-07)
    const Records left  = ThreeSplitRecords(ThreeSplit::Left);
    const Records right = ThreeSplitRecords(ThreeSplit::Right);
    EXPECT(Decide(kStockThreeColumns, left.record, kStockThreeColumns, left.record, kStockThreeColumns) == Action::Write);
    EXPECT(Decide(left.record, right.record, kStockThreeColumns, right.record, left.record) == Action::Write);
    EXPECT(Decide(left.record, kStockThreeColumns, kStockThreeColumns, left.record, left.record) == Action::Restore);
    EXPECT(Decide(right.record, right.record, kStockThreeColumns, right.record, right.record) == Action::Nothing);
    EXPECT(Decide(left.record, right.record, kStockThreeColumns, right.record, kStockThreeColumns) == Action::Foreign);
}
}  // namespace

int main()
{
    StockEntries();
    ColumnsOfTheirWidths();
    ColumnsOfTheirHeights();
    Decisions();
    Wanted();
    Orders();
    SlotsAndEntries();
    MovedOrders();
    TwoViews();
    ThreeSplits();
    return lss::test::Report();
}
