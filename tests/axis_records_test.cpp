// Host-side tests for views::AxisRecords, run without the game.
//
// The FOV rule table: the first split frame stores the original and writes the rule; a change of the mod's rule
// rewrites; a value the game wrote is noted, not overwritten; a full table changes nothing; the restore skips the
// removed player and only puts back a field that still holds a mod rule.

#include "views/axis_records.h"

#include "expect.h"

#include <cstdio>
#include <vector>

namespace {

using lss::views::AxisApplied;
using lss::views::AxisRecords;

int g_a, g_b, g_c;

bool IsModRule(std::uint8_t value)
{
    return value == 1 || value == 2;
}

void TestApply()
{
    std::puts("axis records: first frame, a rule change, the game's writes noted, a full table");
    AxisRecords<2> records;
    std::uint8_t   fieldA = 0, fieldB = 0, fieldC = 0;
    EXPECT(records.Apply(&g_a, fieldA, 2) == AxisApplied::First);
    EXPECT(fieldA == 2 && records.Records()[0].original == 0);
    EXPECT(records.Apply(&g_a, fieldA, 2) == AxisApplied::Unchanged);
    EXPECT(records.Apply(&g_a, fieldA, 1) == AxisApplied::RuleChanged);
    EXPECT(fieldA == 1 && records.Records()[0].applied == 1);

    fieldA = 0;  // a camera cut's value
    EXPECT(records.Apply(&g_a, fieldA, 1) == AxisApplied::GameWrote);
    EXPECT(fieldA == 0);  // left to the game
    EXPECT(records.Apply(&g_a, fieldA, 1) == AxisApplied::Unchanged);  // noted once
    fieldA = 1;
    EXPECT(records.Apply(&g_a, fieldA, 1) == AxisApplied::GamePutBack);
    EXPECT(records.Apply(&g_a, fieldA, 1) == AxisApplied::Unchanged);

    EXPECT(records.Apply(&g_b, fieldB, 1) == AxisApplied::First);
    EXPECT(records.Apply(&g_c, fieldC, 1) == AxisApplied::NoRoom);
    EXPECT(fieldC == 0);
}

void TestRestore()
{
    std::puts("axis records: the restore skips the removed player, puts back only a mod rule, forgets every record");
    AxisRecords<3>       records;
    std::uint8_t         fieldA = 0, fieldB = 0, fieldC = 0;
    std::vector<uint8_t> restored;
    records.Apply(&g_a, fieldA, 2);
    records.Apply(&g_b, fieldB, 2);
    records.Apply(&g_c, fieldC, 2);
    fieldB = 7;  // the game's own value: not the mod's, so not touched
    const auto fieldOf = [&](const void* player) -> std::uint8_t* {
        return player == &g_a ? &fieldA : player == &g_b ? &fieldB : player == &g_c ? &fieldC : nullptr;
    };
    const void* const removed[] = {&g_c};
    records.Restore(removed, fieldOf, &IsModRule, [&](std::uint8_t original) { restored.push_back(original); });
    EXPECT(fieldA == 0);
    EXPECT(fieldB == 7);
    EXPECT(fieldC == 2);  // the removed player's object is gone: not touched
    EXPECT(restored.size() == 1 && restored[0] == 0);
    for (const auto& record : records.Records())
    {
        EXPECT(record.player == nullptr);
    }
    EXPECT(records.Apply(&g_a, fieldA, 2) == AxisApplied::First);  // the next split records afresh
}

void TestRestoreTwoRemoved()
{
    std::puts("axis records: two players removed in one frame are both skipped");
    AxisRecords<3>       records;
    std::uint8_t         fieldA = 0, fieldB = 0, fieldC = 0;
    std::vector<uint8_t> restored;
    records.Apply(&g_a, fieldA, 2);
    records.Apply(&g_b, fieldB, 2);
    records.Apply(&g_c, fieldC, 2);
    const auto fieldOf = [&](const void* player) -> std::uint8_t* {
        return player == &g_a ? &fieldA : player == &g_b ? &fieldB : player == &g_c ? &fieldC : nullptr;
    };
    const void* const removed[] = {&g_b, &g_c};
    records.Restore(removed, fieldOf, &IsModRule, [&](std::uint8_t original) { restored.push_back(original); });
    EXPECT(fieldA == 0 && fieldB == 2 && fieldC == 2);
    EXPECT(restored.size() == 1);
}

}  // namespace

int main()
{
    TestApply();
    TestRestore();
    TestRestoreTwoRemoved();

    return lss::test::Report();
}
