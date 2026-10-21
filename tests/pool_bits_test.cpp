// Host-side tests for binned_pools::CommittedFromLastRow and LiveBlocks, run without the game.

#include "platform/pool_bits.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using lss::platform::binned_pools::CommittedFromLastRow;
using lss::platform::binned_pools::LiveBlocks;

void TestCommitted()
{
    std::puts("pools: committed blocks-of-blocks are the last row's bits less the padding");
    // The 32 KB pool: 8192 blocks-of-blocks, 128 words, no padding.
    std::array<std::uint64_t, 128> full{};
    full.fill(~0ull);
    EXPECT(CommittedFromLastRow(full, 8192) == 8192);
    std::array<std::uint64_t, 128> some{};
    some[0] = 0b1011;
    some[5] = 1ull << 63;
    EXPECT(CommittedFromLastRow(some, 8192) == 4);
    EXPECT(CommittedFromLastRow(std::array<std::uint64_t, 128>{}, 8192) == 0);

    // A pool of 2730 (43 words): the last word's 22 bits past 2730 are pre-set to 1.
    std::array<std::uint64_t, 43> padded{};
    padded[42] = ~0ull << 42;  // padding only
    EXPECT(CommittedFromLastRow(padded, 2730) == 0);
    padded[0] = 0b111;
    EXPECT(CommittedFromLastRow(padded, 2730) == 3);
    padded.fill(~0ull);
    EXPECT(CommittedFromLastRow(padded, 2730) == 2730);
    // A torn read that lost the padding never wraps below zero.
    EXPECT(CommittedFromLastRow(std::array<std::uint64_t, 43>{}, 2730) == 0);
}

void TestLiveBlocks()
{
    std::puts("pools: live blocks sum Taken over the assigned info words only");
    const std::array<std::uint32_t, 4> infos{
        (2u << 2) | 1u,       // assigned, 2 taken
        (1u << 2) | 1u,       // assigned, 1 taken
        (7u << 2) | 3u,       // free: not counted
        (0x7FFFu << 2) | 1u,  // assigned, the field's maximum
    };
    EXPECT(LiveBlocks(infos) == 3 + 0x7FFF);
    EXPECT(LiveBlocks(std::array<std::uint32_t, 0>{}) == 0);
    // Bits above the Taken field (FirstFreeIndex) are ignored.
    const std::array<std::uint32_t, 1> high{(0x3FFFu << 18) | (5u << 2) | 1u};
    EXPECT(LiveBlocks(high) == 5);
}

}  // namespace

int main()
{
    TestCommitted();
    TestLiveBlocks();
    return lss::test::Report();
}
