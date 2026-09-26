// Host-side tests for platform::ForEachSparseElement over a stand-in laid out like a stock UE4 4.26
// TSparseArray: data +0, inline allocation bits +0x10 (4 dwords), heap bits pointer +0x20, NumBits
// +0x28.

#include "platform/sparse_array.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

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

struct Header
{
    std::uintptr_t data = 0;
    std::int32_t   num  = 0;
    std::int32_t   max  = 0;
    std::uint32_t  inlineBits[4]{};
    std::uintptr_t heapBits = 0;
    std::int32_t   numBits  = 0;
    std::int32_t   pad      = 0;
};
static_assert(offsetof(Header, inlineBits) == 0x10 && offsetof(Header, heapBits) == 0x20 && offsetof(Header, numBits) == 0x28);

constexpr std::uint32_t kElement = 12;

/// The indices visited, in order, stopping after `stopAfter` visits (0: never).
std::vector<int> Walk(const Header& header, int stopAfter = 0)
{
    std::vector<int> seen;
    lss::platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(&header), kElement, [&](std::uintptr_t element) {
        seen.push_back(static_cast<int>((element - header.data) / kElement));
        return stopAfter == 0 || static_cast<int>(seen.size()) < stopAfter;
    });
    return seen;
}

void InlineBits(const std::uintptr_t data)
{
    std::puts("sparse array: inline bits, order, free slots, the last partial word");
    Header header;
    header.data          = data;
    header.inlineBits[0] = (1u << 0) | (1u << 5) | (1u << 31);
    header.inlineBits[1] = 0;                        // a whole free word
    header.inlineBits[2] = (1u << 0) | (1u << 3);    // slots 64 and 67
    header.numBits       = 67;                       // slot 67 is past NumBits
    EXPECT((Walk(header) == std::vector<int>{0, 5, 31, 64}));
}

void HeapBits(const std::uintptr_t data)
{
    std::puts("sparse array: heap bits win over inline ones");
    std::array<std::uint32_t, 3> heap{0, 1u << 1, 1u << 2};
    Header header;
    header.data          = data;
    header.inlineBits[0] = 0xFFFFFFFFu;  // ignored while the heap pointer is set
    header.heapBits      = reinterpret_cast<std::uintptr_t>(heap.data());
    header.numBits       = 96;
    EXPECT((Walk(header) == std::vector<int>{33, 66}));
}

void Stops(const std::uintptr_t data)
{
    std::puts("sparse array: a visit returning false ends the walk");
    Header header;
    header.data          = data;
    header.inlineBits[0] = 0xFu;
    header.inlineBits[1] = 0x1u;
    header.numBits       = 40;
    EXPECT((Walk(header, 2) == std::vector<int>{0, 1}));
    EXPECT((Walk(header) == std::vector<int>{0, 1, 2, 3, 32}));
}

void Refuses(const std::uintptr_t data)
{
    std::puts("sparse array: no data, no bits or too many bits visit nothing");
    Header header;
    header.inlineBits[0] = 1;
    header.numBits       = 1;
    EXPECT(Walk(header).empty());  // data null
    header.data    = data;
    header.numBits = 0;
    EXPECT(Walk(header).empty());
    header.numBits = lss::platform::kSparseMaxBits + 1;
    EXPECT(Walk(header).empty());
}

}  // namespace

int main()
{
    std::vector<std::uint8_t> elements(128 * kElement);
    const auto                data = reinterpret_cast<std::uintptr_t>(elements.data());
    InlineBits(data);
    HeapBits(data);
    Stops(data);
    Refuses(data);
    std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
