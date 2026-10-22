// Host-side tests for platform::HashFind and WalkFind (platform/tset_find.h) over a stand-in laid out like a stock
// UE4 4.26 TSet keyed by an FName: the element array (data +0, ArrayNum +8, inline allocation bits +0x10, heap bits
// +0x20, NumBits +0x28, NumFreeIndices +0x34), one inline hash bucket +0x38, the buckets' heap pointer +0x40 and
// HashSize +0x48; an element of 24 bytes with the FName at +0 and its chain's next index at +16.

#include "platform/tset_find.h"

#include "expect.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using lss::platform::FNameHash;
using lss::platform::HashFind;
using lss::platform::SetShape;
using lss::platform::WalkFind;

struct Set
{
    std::uintptr_t data = 0;
    std::int32_t   num  = 0;
    std::int32_t   max  = 0;
    std::uint32_t  inlineBits[4]{};
    std::uintptr_t heapBits   = 0;
    std::int32_t   numBits    = 0;
    std::int32_t   maxBits    = 0;
    std::int32_t   firstFree  = -1;
    std::int32_t   numFree    = 0;
    std::int32_t   inlineHash = -1;
    std::int32_t   pad        = 0;
    std::uintptr_t heapHash   = 0;
    std::int32_t   hashSize   = 0;
    std::int32_t   pad2       = 0;
};
static_assert(offsetof(Set, inlineBits) == 0x10 && offsetof(Set, heapBits) == 0x20 && offsetof(Set, numBits) == 0x28 &&
              offsetof(Set, numFree) == 0x34 && offsetof(Set, inlineHash) == 0x38 && offsetof(Set, heapHash) == 0x40 &&
              offsetof(Set, hashSize) == 0x48);

struct Element
{
    std::uint64_t key      = 0;
    std::uint64_t value    = 0;
    std::int32_t  hashNext = -1;
    std::int32_t  pad      = 0;
};
constexpr SetShape kShape{sizeof(Element), offsetof(Element, hashNext)};

/// An FName value: the comparison index in the low word, the number in the high.
constexpr std::uint64_t Name(std::uint32_t index, std::uint32_t number = 0)
{
    return (static_cast<std::uint64_t>(number) << 32) | index;
}

/// A set holding `keys`, hashed into `bucketCount` buckets (on the heap when more than one), the chains built as the
/// engine builds them (each new element at the head of its bucket's chain).
struct Fixture
{
    std::vector<Element>      elements;
    std::vector<std::int32_t> buckets;
    Set                       set;

    explicit Fixture(const std::vector<std::uint64_t>& keys, std::int32_t bucketCount = 8)
        : elements(keys.size()), buckets(static_cast<std::size_t>(bucketCount), -1)
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            elements[i].key = keys[i];
            auto& bucket    = buckets[FNameHash(keys[i]) & static_cast<std::uint32_t>(bucketCount - 1)];
            elements[i].hashNext = bucket;
            bucket               = static_cast<std::int32_t>(i);
        }
        set.data    = reinterpret_cast<std::uintptr_t>(elements.data());
        set.num     = static_cast<std::int32_t>(keys.size());
        set.max     = set.num;
        set.numBits = set.num;
        set.maxBits = set.num;
        for (std::int32_t i = 0; i < set.num; ++i)
        {
            set.inlineBits[i / 32] |= 1u << (i % 32);
        }
        set.hashSize = bucketCount;
        if (bucketCount == 1)
        {
            set.inlineHash = buckets[0];
        }
        else
        {
            set.heapHash = reinterpret_cast<std::uintptr_t>(buckets.data());
        }
    }

    std::uintptr_t At() const { return reinterpret_cast<std::uintptr_t>(&set); }
};

void Finds()
{
    std::puts("tset find: every key by the hash and by the walk; an absent key by neither");
    std::vector<std::uint64_t> keys;
    for (std::uint32_t i = 1; i <= 40; ++i)
    {
        keys.push_back(Name(i * 977, i % 3));  // several per bucket: the chains are walked
    }
    const Fixture f(keys);
    for (const std::uint64_t key : keys)
    {
        EXPECT(HashFind(f.At(), kShape, key));
        EXPECT(WalkFind(f.At(), kShape, key));
    }
    EXPECT(!HashFind(f.At(), kShape, Name(12345)));
    EXPECT(!WalkFind(f.At(), kShape, Name(12345)));
    // The same index with another number is another name; Name(977, 9) shares the bucket of Name(977, 1), so the
    // chain is walked past it and the full key compared.
    EXPECT((FNameHash(Name(977, 9)) & 7) == (FNameHash(Name(977, 1)) & 7));
    EXPECT(!HashFind(f.At(), kShape, Name(977, 9)));
}

void LargeHash()
{
    std::puts("tset find: more buckets than elements (the mask is the hash size, not the count)");
    const Fixture f({Name(3), Name(300), Name(30000)}, 64);
    EXPECT(HashFind(f.At(), kShape, Name(3)));
    EXPECT(HashFind(f.At(), kShape, Name(300)));
    EXPECT(HashFind(f.At(), kShape, Name(30000)));
    EXPECT(!HashFind(f.At(), kShape, Name(4)));
}

void OneInlineBucket()
{
    std::puts("tset find: a set with one bucket keeps it inline");
    const Fixture f({Name(5), Name(6), Name(7)}, 1);
    EXPECT(f.set.heapHash == 0);
    EXPECT(HashFind(f.At(), kShape, Name(6)));
    EXPECT(!HashFind(f.At(), kShape, Name(8)));
}

void Empty()
{
    std::puts("tset find: an empty set, and one whose elements are all free, find nothing");
    const Fixture none({});
    EXPECT(!HashFind(none.At(), kShape, Name(1)));
    EXPECT(!WalkFind(none.At(), kShape, Name(1)));
    Fixture freed({Name(1), Name(2)});
    freed.set.numFree = 2;  // ArrayNum == NumFreeIndices: nothing allocated, whatever the buckets say
    EXPECT(!HashFind(freed.At(), kShape, Name(1)));
}

void FreedSlot()
{
    std::puts("tset find: a freed slot is skipped by the walk");
    Fixture f({Name(1), Name(2), Name(3)});
    f.set.inlineBits[0] &= ~(1u << 1);  // slot 1 freed: skipped whatever its bytes say (a live set keeps free links there)
    f.set.numFree = 1;
    EXPECT(WalkFind(f.At(), kShape, Name(1)));
    EXPECT(!WalkFind(f.At(), kShape, Name(2)));
    EXPECT(WalkFind(f.At(), kShape, Name(3)));
}

void BadHeader()
{
    std::puts("tset find: a hash size that is not a power of two, no elements or a null set find nothing by the hash");
    Fixture f({Name(1), Name(2)});
    f.set.hashSize = 6;
    EXPECT(!HashFind(f.At(), kShape, Name(1)));
    EXPECT(WalkFind(f.At(), kShape, Name(1)));  // the walk does not read the buckets
    f.set.hashSize = 0;
    EXPECT(!HashFind(f.At(), kShape, Name(1)));
    f.set.hashSize = 8;
    f.set.data     = 0;
    EXPECT(!HashFind(f.At(), kShape, Name(1)));
    EXPECT(!HashFind(0, kShape, Name(1)));
    EXPECT(!WalkFind(0, kShape, Name(1)));
}

void Cycle()
{
    std::puts("tset find: a chain that loops, or names an index past the bound, ends");
    Fixture f({Name(1), Name(2)});  // Name(1) heads bucket 1 as element 0, Name(2) bucket 2 as element 1
    f.elements[0].hashNext = 1;  // 0 -> 1 -> 0 ...
    f.elements[1].hashNext = 0;
    EXPECT((FNameHash(Name(9)) & 7) == 1);  // an absent key on the looping bucket: walked until the bound
    EXPECT(HashFind(f.At(), kShape, Name(1)));
    EXPECT(!HashFind(f.At(), kShape, Name(9), 16));
    f.elements[0].hashNext = 20000;  // an index past the bound ends the chain without a read there
    EXPECT(!HashFind(f.At(), kShape, Name(9)));
}

void Hash()
{
    std::puts("tset find: the FName hash mixes the number and the index's halves as the game does");
    EXPECT(FNameHash(Name(0)) == 0);
    EXPECT(FNameHash(Name(0, 1)) == 1);
    EXPECT(FNameHash(Name(1)) == 1 + (1u << 16));
    EXPECT(FNameHash(Name(0x10000)) == 1 + (8u << 16));
    EXPECT(FNameHash(Name(16)) == 16 + 1 + (16u << 16));
}

}  // namespace

int main()
{
    Finds();
    LargeHash();
    OneInlineBucket();
    Empty();
    FreedSlot();
    BadHeader();
    Cycle();
    Hash();
    return lss::test::Report();
}
