// Host-side tests for battle/battle_rates_rules.h: the multipliers' words and the game's rounding. Run without the game.

#include "battle/battle_rates_rules.h"

#include "expect.h"

#include <climits>
#include <cstdio>
#include <cwchar>

namespace {

using namespace lss::battle::battle_rates_rules;

void TestWordsRoundTrip()
{
    std::puts("the words: 0.1 to 10.0, each its tenths");
    EXPECT(TenthsOf(L"0.1") == 1);
    EXPECT(TenthsOf(L"1.0") == kOne);
    EXPECT(TenthsOf(L"2.5") == 25);
    EXPECT(TenthsOf(L"10.0") == 100);
    EXPECT(!TenthsOf(L"0.0"));
    EXPECT(!TenthsOf(L"11.0"));
    EXPECT(!TenthsOf(L"one"));
    for (int t = 1; t <= 100; ++t)
    {
        EXPECT(TenthsOf(WordOf(t)) == t);
    }
    EXPECT(std::wcscmp(WordOf(0), L"1.0") == 0);
    EXPECT(std::wcscmp(WordOf(101), L"1.0") == 0);
}

void TestScaledRoundsAsTheGame()
{
    std::puts("scaled values round half up as the game's rates, at least 1");
    EXPECT(Scaled(100, kOne) == 100);
    EXPECT(Scaled(100, 15) == 150);
    EXPECT(Scaled(105, 15) == 158);  // 157.5 rounds up
    EXPECT(Scaled(3, 1) == 1);       // 0.3 -> at least 1
    EXPECT(Scaled(0, 20) == 0);
    EXPECT(Scaled(-5, 20) == -5);
    EXPECT(Scaled(INT_MAX / 2, 100) == INT_MAX / 2);  // no overflow
}

void TestTheMultiplier()
{
    std::puts("the multiplier the game's step takes");
    EXPECT(Multiplier(kOne) == 1.0f);
    EXPECT(Multiplier(5) == 0.5f);
}

void TestAwardPercent()
{
    std::puts("the AP bonus row's percent: the game's while its bonus applies, else 100, times the rate");
    EXPECT(AwardPercent(200, true, kOne) == 200);
    EXPECT(AwardPercent(200, false, kOne) == 100);
    EXPECT(AwardPercent(200, true, 15) == 300);
    EXPECT(AwardPercent(200, false, 25) == 250);
    EXPECT(AwardPercent(300, true, 1) == 30);
}

}  // namespace

int main()
{
    TestWordsRoundTrip();
    TestScaledRoundsAsTheGame();
    TestTheMultiplier();
    TestAwardPercent();
    return lss::test::Report();
}
