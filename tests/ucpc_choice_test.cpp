// Host-side tests for battle::ucpc_choice, run without the game.
//
// Player 2 not in: untouched. Both eligible: the coin decides. One eligible: that one, whatever the coin. Neither:
// player 1, as the game would. The per-seat rule gives the two-way rule's answer for two players over every input and
// coin (the reference kept verbatim below), and a uniform pick among the eligible for three.

#include "battle/ucpc_choice.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::ucpc_choice;

constexpr bool kCoins[] = {false, true};

bool Is(Choice choice, Ucpc ucpc, Why why)
{
    return choice.ucpc == ucpc && choice.why == why;
}

/// The per-seat rule asked the two-player question: player 2 in or not, each eligible or not, the coin's top bit.
constexpr Choice TwoWay(bool playerTwoIn, bool oneEligible, bool twoEligible, bool coin) noexcept
{
    constexpr Players kTwo = Bit(lss::players::kFirstFurtherSeat);
    return Choose(static_cast<Players>(Bit(0) | (playerTwoIn ? kTwo : 0)),
                  static_cast<Players>((oneEligible ? Bit(0) : 0) | (twoEligible ? kTwo : 0)),
                  coin ? ~std::uint64_t{0} : 0);
}

void TestNoPlayerTwo()
{
    std::puts("ucpc choice: without player 2 the game's answer is untouched");
    for (int bits = 0; bits < 8; ++bits)
    {
        EXPECT(Is(TwoWay(false, bits & 1, bits & 2, bits & 4), Ucpc::Untouched, Why::NoSeat));
    }
}

void TestBothEligible()
{
    std::puts("ucpc choice: both eligible, the coin decides");
    EXPECT(Is(TwoWay(true, true, true, false), Ucpc::PlayerOne, Why::Coin));
    EXPECT(Is(TwoWay(true, true, true, true), Ucpc::Seat, Why::Coin));
}

void TestOneEligible()
{
    std::puts("ucpc choice: one eligible takes it, whatever the coin");
    for (const bool coin : kCoins)
    {
        EXPECT(Is(TwoWay(true, true, false, coin), Ucpc::PlayerOne, Why::OnlyEligible));
        EXPECT(Is(TwoWay(true, false, true, coin), Ucpc::Seat, Why::OnlyEligible));
    }
}

void TestNeitherEligible()
{
    std::puts("ucpc choice: neither eligible, player 1 as the game would");
    for (const bool coin : kCoins)
    {
        EXPECT(Is(TwoWay(true, false, false, coin), Ucpc::PlayerOne, Why::NeitherEligible));
    }
}

static_assert(TwoWay(true, true, true, true).ucpc == Ucpc::Seat);

// ---- the per-seat rule against the two-way coin it replaced ----

/// The two-way rule as it stood at v0.91.0, verbatim, as the reference the per-seat rule must reproduce for two
/// players: `coin` true picks player 2.
constexpr Choice OldChoose(bool playerTwoIn, bool oneEligible, bool twoEligible, bool coin) noexcept
{
    if (!playerTwoIn)
    {
        return {Ucpc::Untouched, Why::NoSeat};
    }
    if (oneEligible && twoEligible)
    {
        return {coin ? Ucpc::Seat : Ucpc::PlayerOne, Why::Coin};
    }
    if (oneEligible || twoEligible)
    {
        return {twoEligible ? Ucpc::Seat : Ucpc::PlayerOne, Why::OnlyEligible};
    }
    return {Ucpc::PlayerOne, Why::NeitherEligible};
}

/// The coin values tried: both ends of each half, the middle, and a spread from a fixed splitmix64 stream (the
/// stream equal_targeting draws from).
std::uint64_t CoinAt(int i)
{
    constexpr std::uint64_t kEdges[] = {0, 1, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull,
                                        0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull, 0x5555555555555555ull,
                                        0xAAAAAAAAAAAAAAAAull};
    constexpr int           kEdgeCount = static_cast<int>(sizeof(kEdges) / sizeof(kEdges[0]));
    if (i < kEdgeCount)
    {
        return kEdges[i];
    }
    std::uint64_t x = 0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(i + 1);
    x               = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x               = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr int kCoinsTried = 4096;

void TestTwoPlayersAsBefore()
{
    std::puts("ucpc choice: with two players the per-seat rule gives the two-way coin's answer for every input and "
              "coin (its top bit)");
    constexpr Players kTwo = Bit(lss::players::kFirstFurtherSeat);
    for (int bits = 0; bits < 8; ++bits)
    {
        const bool    in       = (bits & 1) != 0;
        const bool    one      = (bits & 2) != 0;
        const bool    two      = (bits & 4) != 0;
        const Players inSet    = static_cast<Players>(Bit(0) | (in ? kTwo : 0));
        const Players eligible = static_cast<Players>((one ? Bit(0) : 0) | (two ? kTwo : 0));
        // The old caller drew a coin only with both eligible; the new one only when NeedsCoin.
        EXPECT(NeedsCoin(inSet, eligible) == (in && one && two));
        for (int i = 0; i < kCoinsTried; ++i)
        {
            const std::uint64_t coin   = CoinAt(i);
            const bool          bit    = (coin >> 63) != 0;  // equal_targeting's old Coin(): the top bit
            const Choice        old    = OldChoose(in, one, two, in && one && two && bit);
            const Choice        now    = Choose(inSet, eligible, coin);
            const Choice        shortc = TwoWay(in, one, two, in && one && two && bit);
            EXPECT(now.ucpc == old.ucpc && now.why == old.why);
            EXPECT(shortc.ucpc == old.ucpc && shortc.why == old.why && shortc.player == now.player);
            EXPECT(now.player == (old.ucpc == Ucpc::Untouched ? kUntouched
                                  : old.ucpc == Ucpc::Seat    ? lss::players::kFirstFurtherSeat
                                                              : 0));
        }
    }
}

void TestThreePlayers()
{
    std::puts("ucpc choice: three players, a uniform pick among the eligible; one eligible takes it; none, player 1");
    constexpr Players kAll = 0b111;
    for (Players eligible = 0; eligible < 8; ++eligible)
    {
        int seen[3] = {};
        for (int i = 0; i < kCoinsTried; ++i)
        {
            const Choice choice = Choose(kAll, eligible, CoinAt(i));
            EXPECT(choice.player >= 0 && choice.player < 3);
            EXPECT((choice.player == 0) == (choice.ucpc == Ucpc::PlayerOne));
            if (eligible != 0)
            {
                EXPECT((eligible & Bit(choice.player)) != 0);  // only an eligible player is picked
            }
            ++seen[choice.player];
        }
        const int count = Count(eligible);
        EXPECT(Choose(kAll, eligible, 0).why ==
               (count >= 2 ? Why::Coin : count == 1 ? Why::OnlyEligible : Why::NeitherEligible));
        if (count == 0)
        {
            EXPECT(seen[0] == kCoinsTried);
        }
        for (int player = 0; player < 3; ++player)
        {
            if (count >= 2 && (eligible & Bit(player)) != 0)
            {
                // Uniform: each of the eligible within 10 % of its share over the spread.
                const int share = kCoinsTried / count;
                EXPECT(seen[player] > share - share / 10 && seen[player] < share + share / 10);
            }
            else if (count == 1)
            {
                EXPECT(seen[player] == ((eligible & Bit(player)) != 0 ? kCoinsTried : 0));
            }
        }
    }
    // Player 3 in, player 2 out: a two-way coin between players 1 and 3 by the top bit.
    EXPECT(Choose(0b101, 0b111, 0).player == 0);
    EXPECT(Choose(0b101, 0b111, ~std::uint64_t{0}).player == 2);
    EXPECT(Choose(0b101, 0b010, ~std::uint64_t{0}).player == 0);  // player 2 is not in: not a candidate
    EXPECT(Choose(0b001, 0b111, ~std::uint64_t{0}).ucpc == Ucpc::Untouched);
    EXPECT(!NeedsCoin(0b111, 0b100) && NeedsCoin(0b111, 0b110) && !NeedsCoin(0b001, 0b111));
}

void TestUniformIndex()
{
    std::puts("ucpc choice: the uniform index is the high word of coin x count");
    EXPECT(UniformIndex(0, 3) == 0 && UniformIndex(~std::uint64_t{0}, 3) == 2);
    EXPECT(UniformIndex(0x5555555555555555ull, 3) == 0 && UniformIndex(0x5555555555555556ull, 3) == 1);
    EXPECT(UniformIndex(0xAAAAAAAAAAAAAAABull, 3) == 2 && UniformIndex(0xAAAAAAAAAAAAAAAAull, 3) == 1);
    for (int i = 0; i < kCoinsTried; ++i)
    {
        EXPECT(UniformIndex(CoinAt(i), 2) == static_cast<int>(CoinAt(i) >> 63));
        EXPECT(UniformIndex(CoinAt(i), 1) == 0);
    }
    EXPECT(Nth(0b110, 0) == 1 && Nth(0b110, 1) == 2 && Nth(0b110, 2) == kUntouched);
}

}  // namespace

int main()
{
    TestNoPlayerTwo();
    TestBothEligible();
    TestOneEligible();
    TestNeitherEligible();
    TestTwoPlayersAsBefore();
    TestThreePlayers();
    TestUniformIndex();
    return lss::test::Report();
}
