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
    EXPECT(Decide(kStockThreeColumns, columns.record, columns.record, kStockThreeColumns) == Action::Write);
    EXPECT(Decide(kStockThreeColumns, kStockThreeColumns, columns.record, kStockThreeColumns) == Action::Nothing);
    EXPECT(Decide(columns.record, columns.record, columns.record, columns.record) == Action::Nothing);
    EXPECT(Decide(columns.record, kStockThreeColumns, columns.record, columns.record) == Action::Restore);
    EXPECT(Decide(columns.record, other.record, other.record, columns.record) == Action::Write);    // the row changed
    EXPECT(Decide(columns.record, other.record, other.record, kStockThreeColumns) == Action::Foreign);  // never written
    Record foreign[kThreePlayerRecords] = {{0.5f, 1.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT(Decide(foreign, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    EXPECT(Decide(foreign, kStockThreeColumns, columns.record, kStockThreeColumns) == Action::Foreign);
    Record zero[kThreePlayerRecords] = {};
    EXPECT(Decide(zero, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    // Partly the mod's (two records written, the third stock): foreign, never "restored" over.
    Record partly[kThreePlayerRecords] = {columns.record[0], columns.record[1], kStockThreeColumns[2]};
    EXPECT(Decide(partly, kStockThreeColumns, columns.record, kStockThreeColumns) == Action::Foreign);
    EXPECT(Decide(partly, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    // A record twice is no arrangement: foreign.
    Record twice[kThreePlayerRecords] = {columns.record[0], columns.record[1], columns.record[1]};
    EXPECT(Decide(twice, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    // The FavorTop entry's records in the three-column slot: foreign.
    Record favorTop[kThreePlayerRecords] = {{1.0f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};
    EXPECT(Decide(favorTop, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    // A NaN never equals anything: foreign, left alone.
    Record nan[kThreePlayerRecords] = {kStockThreeColumns[0], kStockThreeColumns[1],
                                       {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, std::numeric_limits<float>::quiet_NaN()}};
    EXPECT(Decide(nan, columns.record, columns.record, kStockThreeColumns) == Action::Foreign);
    // Negative zero equals zero: still the stock entry.
    Record negativeZero[kThreePlayerRecords] = {{1.0f / 3.0f, 1.0f, -0.0f, -0.0f}, kStockThreeColumns[1], kStockThreeColumns[2]};
    EXPECT(Decide(negativeZero, columns.record, columns.record, kStockThreeColumns) == Action::Write);
    // The stock records in another order (an arrangement): one of the known entries, restored to the stock order.
    Record swapped[kThreePlayerRecords] = {kStockThreeColumns[0], kStockThreeColumns[2], kStockThreeColumns[1]};
    EXPECT(Decide(swapped, kStockThreeColumns, columns.record, kStockThreeColumns) == Action::Restore);
    EXPECT(Decide(swapped, columns.record, columns.record, kStockThreeColumns) == Action::Write);

    // Two players on a two-monitor span: the side-by-side entry from the two widths.
    const TwoRecords side = ColumnRecords(std::array<std::int32_t, 2>{3440, 2560});
    EXPECT(DecideTwo(kStockSideBySide, side.record, kStockSideBySide, side.record, kStockSideBySide) == Action::Write);
    EXPECT(DecideTwo(side.record, kStockSideBySide, kStockSideBySide, side.record, side.record) == Action::Restore);
    EXPECT(DecideTwo(side.record, side.record, kStockSideBySide, kStockSideBySide, side.record) == Action::Nothing);
    EXPECT(DecideTwo(side.record, kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Foreign);
    EXPECT(DecideTwo(kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Nothing);
    EXPECT(DecideTwo(kStockTopBottom, side.record, kStockSideBySide, side.record, kStockSideBySide) == Action::Foreign);
}

/// The entries wanted: the span's records only while the engine lists as many local players as the entry lays out.
void Wanted()
{
    std::puts("column rules: the span's records while the players are listed, the stock ones otherwise");
    const Records columns = ColumnRecords(std::array<std::int32_t, 3>{2560, 3440, 2560});
    EXPECT(Same(ThreeWanted(columns, 3).record, columns.record));
    EXPECT(Same(ThreeWanted(columns, 2).record, kStockThreeColumns));  // two listed: the entry is not read
    EXPECT(Same(ThreeWanted(columns, 1).record, kStockThreeColumns));
    const Records stock{{kStockThreeColumns[0], kStockThreeColumns[1], kStockThreeColumns[2]}};
    EXPECT(Same(ThreeWanted(stock, 3).record, kStockThreeColumns));  // one window, or equal monitors: nothing to write
    const TwoRecords side = ColumnRecords(std::array<std::int32_t, 2>{3440, 2560});
    EXPECT(Same(TwoWanted(side, kStockSideBySide, 2).record, side.record));
    EXPECT(Same(TwoWanted(side, kStockSideBySide, 3).record, kStockSideBySide));  // three listed: the two-player entries idle
    EXPECT(Same(TwoWanted(side, kStockSideBySide, 1).record, kStockSideBySide));
    static_assert(ThreeWanted(Records{}, 1).record[0] == kStockThreeColumns[0], "constexpr");
}

}  // namespace

int main()
{
    StockEntries();
    ColumnsOfTheirWidths();
    ColumnsOfTheirHeights();
    Decisions();
    Wanted();
    return lss::test::Report();
}
