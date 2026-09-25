// Host-side tests for player_two::SelectPadSticks, run without the game.
//
// Two pads in the XInput bank: pad 0 pushes left, pad 1 pushes up. Each pad must read only its own
// record, per stick, and a merged stick of exactly zero must stay zero.

#include "player_two/stick_select.h"

#include <array>
#include <cstdio>

namespace {

using lss::player_two::SelectPadSticks;
using lss::player_two::StickBank;
using lss::player_two::Sticks;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

//                                        LX     LY    RX    RY    LT   RT
constexpr std::array<float, 12> kXInput{-1.0f, 0.0f, 0.0f, 0.3f, 0.0f, 0.0f,   // pad 0
                                         0.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f};  // pad 1
constexpr Sticks kMerged{-1.0f, 0.0f, 0.5f, 0.0f};  // the game's merge: pad 0's left, pad 1's right

void TestEachPadReadsItsOwnRecord()
{
    std::puts("sticks: each pad reads only its own record");
    const std::array<StickBank, 1> banks{StickBank{kXInput.data(), 2}};
    EXPECT((SelectPadSticks(banks, 0, kMerged) == Sticks{-1.0f, 0.0f, 0.0f, 0.3f}));
    EXPECT((SelectPadSticks(banks, 1, kMerged) == Sticks{0.0f, 1.0f, 0.5f, 0.0f}));
}

void TestMissingPadIsIdle()
{
    std::puts("sticks: a pad with no record, or an invalid id, reads as idle");
    const std::array<StickBank, 1> banks{StickBank{kXInput.data(), 2}};
    EXPECT((SelectPadSticks(banks, 2, kMerged) == Sticks{}));
    EXPECT((SelectPadSticks(banks, -1, kMerged) == Sticks{}));
    const std::array<StickBank, 1> empty{StickBank{nullptr, 4}};
    EXPECT((SelectPadSticks(empty, 0, kMerged) == Sticks{}));
}

void TestZeroMergeStaysZero()
{
    std::puts("sticks: a merged stick of exactly zero stays zero (the game's unfocused rule)");
    const std::array<StickBank, 1> banks{StickBank{kXInput.data(), 2}};
    EXPECT((SelectPadSticks(banks, 1, Sticks{0.0f, 0.0f, 0.5f, 0.0f}) == Sticks{0.0f, 0.0f, 0.5f, 0.0f}));
    EXPECT((SelectPadSticks(banks, 0, Sticks{}) == Sticks{}));
}

void TestLargestAcrossBanksPerStick()
{
    std::puts("sticks: across banks, each stick takes the pad's largest record");
    constexpr std::array<float, 6> other{0.0f, -0.2f, 0.9f, 0.0f, 0.0f, 0.0f};  // pad 0 in a second bank
    const std::array<StickBank, 2> banks{StickBank{kXInput.data(), 2}, StickBank{other.data(), 1}};
    EXPECT((SelectPadSticks(banks, 0, kMerged) == Sticks{-1.0f, 0.0f, 0.9f, 0.0f}));
}

}  // namespace

int main()
{
    TestEachPadReadsItsOwnRecord();
    TestMissingPadIsIdle();
    TestZeroMergeStaysZero();
    TestLargestAcrossBanksPerStick();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
