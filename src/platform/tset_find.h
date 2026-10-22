#pragma once

// Looks a key up in an Unreal TSet or TMap keyed by an FName by the set's own hash, by reading memory only, and
// walks its allocated elements as the check of that lookup and the fallback.
//
// Stock UE4 4.26 TSet, x64 (read in the game's own lookups, RE-p2-field-interact.md §10.5): the element array is a
// TSparseArray at +0 (platform/sparse_array.h; +0x08 ArrayNum, +0x34 NumFreeIndices), then the hash buckets: +0x38
// one inline bucket, +0x40 their heap pointer (used when non-null), +0x48 HashSize (a power of two). A bucket holds
// the index of its chain's first element; each element holds the next index (HashNextId, an i32) at the shape's
// offset, -1 ending the chain. The key is the FName's two words at element +0; its hash is GetTypeHash(FName) as the
// game computes it: the Number word plus a mix of the ComparisonIndex's halves. Every read is guarded: a header that
// does not read as a set finds nothing. Game thread (the sets belong to game-thread systems).

#include "platform/guarded_read.h"
#include "platform/sparse_array.h"

#include <cstddef>
#include <cstdint>

namespace lss::platform {

inline constexpr std::uint32_t kSetCount      = 0x08;  // i32 ArrayNum
inline constexpr std::uint32_t kSetFreeCount  = 0x34;  // i32 NumFreeIndices
inline constexpr std::uint32_t kSetHashInline = 0x38;
inline constexpr std::uint32_t kSetHashHeap   = 0x40;
inline constexpr std::uint32_t kSetHashSize   = 0x48;  // i32
inline constexpr std::int32_t  kSetMaxItems   = 16384;  // the bound on a chain and on the walk (the world's maps)

/// An element's layout: its size, and where its hash chain's next index (an i32) sits in it.
struct SetShape
{
    std::uint32_t stride;
    std::uint32_t hashNext;
};

/// GetTypeHash of an FName (ComparisonIndex in the low word, Number in the high), as the game's lookups compute it.
constexpr std::uint32_t FNameHash(std::uint64_t name) noexcept
{
    const auto index = static_cast<std::uint32_t>(name);
    const auto lo    = index & 0xFFFF;
    const auto hi    = index >> 16;
    return static_cast<std::uint32_t>(name >> 32) + hi + lo + (lo >> 4) + ((lo + 8 * hi) << 16);
}

/// True if the set at `set` holds an element whose FName at +0 is `key`, by the set's hash. False for an empty set,
/// a header that does not read as one (no elements, a hash size that is not a power of two) or a chain that runs
/// past `maxItems` elements.
inline bool HashFind(std::uintptr_t set, SetShape shape, std::uint64_t key,
                     std::int32_t maxItems = kSetMaxItems) noexcept
{
    if (set == 0)
    {
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(set);
    if (ReadField<std::int32_t>(base, kSetCount) == ReadField<std::int32_t>(base, kSetFreeCount))
    {
        return false;
    }
    const auto* elements = static_cast<const std::uint8_t*>(ReadPointer(base, kSparseData));
    const auto  size     = ReadField<std::int32_t>(base, kSetHashSize);
    const auto* heap     = static_cast<const std::uint8_t*>(ReadPointer(base, kSetHashHeap));
    const auto* buckets  = heap != nullptr ? heap : base + kSetHashInline;
    if (elements == nullptr || size <= 0 || (size & (size - 1)) != 0)
    {
        return false;
    }
    auto i = ReadField<std::int32_t>(buckets, 4 * (FNameHash(key) & static_cast<std::uint32_t>(size - 1)));
    for (std::int32_t steps = 0; i >= 0 && i < maxItems && steps < maxItems; ++steps)
    {
        const auto* element = elements + static_cast<std::size_t>(i) * shape.stride;
        if (ReadField<std::uint64_t>(element, 0) == key)
        {
            return true;
        }
        i = ReadField<std::int32_t>(element, shape.hashNext);
    }
    return false;
}

/// The same by a walk of the set's allocated elements: the check of HashFind, and the lookup once it is distrusted.
inline bool WalkFind(std::uintptr_t set, SetShape shape, std::uint64_t key,
                     std::int32_t maxItems = kSetMaxItems) noexcept
{
    bool found = false;
    ForEachSparseElement(
        set, shape.stride,
        [&found, key](std::uintptr_t element) noexcept {
            found = ReadField<std::uint64_t>(reinterpret_cast<const void*>(element), 0) == key;
            return !found;
        },
        maxItems);
    return found;
}

}  // namespace lss::platform
