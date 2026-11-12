#pragma once

// Walks the allocated elements of an Unreal TSparseArray (the element array of a TSet or TMap) by
// reading memory only.
//
// Stock UE4 4.26 layout, x64 (read in the game's own walks, TConstSetBitIterator, and the sets of
// research/RE-p2-enemy-markers.md and RE-p2-battle-popups.md):
//   +0x00 Data (element pointer), +0x08 ArrayNum,
//   +0x10 allocation bits, inline (4 dwords), +0x20 their heap pointer (used when non-null),
//   +0x28 NumBits.
// An element is allocated when its bit is set. Every read is guarded; a walk that cannot read the
// header visits nothing. Game thread only (the sets belong to game-thread systems).

#include "platform/guarded.h"

#include <cstdint>

namespace lss::platform {

inline constexpr std::uint32_t kSparseData       = 0x00;
inline constexpr std::uint32_t kSparseInlineBits = 0x10;
inline constexpr std::uint32_t kSparseHeapBits   = 0x20;
inline constexpr std::uint32_t kSparseNumBits    = 0x28;
inline constexpr std::int32_t  kSparseMaxBits    = 4096;  // far above any HUD set; guards a bad read

/// Calls `visit(elementAddress)` for each allocated element of the TSparseArray at `array`, in index
/// order, until it returns false. A bit count over `maxBits` is a bad read: nothing is visited (the
/// HUD's sets stay far under the default; a world set passes its own bound). May throw what `visit`
/// throws.
template <class Visit>
void ForEachSparseElement(std::uintptr_t array, std::uint32_t elementSize, Visit&& visit,
                          std::int32_t maxBits = kSparseMaxBits)
{
    std::uintptr_t data = 0, heapBits = 0;
    std::int32_t   numBits = 0;
    if (array == 0 || !CopyGuarded(&data, reinterpret_cast<const void*>(array + kSparseData), sizeof(data)) || data == 0 ||
        !CopyGuarded(&heapBits, reinterpret_cast<const void*>(array + kSparseHeapBits), sizeof(heapBits)) ||
        !CopyGuarded(&numBits, reinterpret_cast<const void*>(array + kSparseNumBits), sizeof(numBits)) || numBits <= 0 ||
        numBits > maxBits)
    {
        return;
    }
    const std::uintptr_t bits = heapBits != 0 ? heapBits : array + kSparseInlineBits;
    for (std::int32_t base = 0; base < numBits; base += 32)
    {
        // One read per 32 slots; a word with no bit set is skipped whole.
        std::uint32_t word = 0;
        if (!CopyGuarded(&word, reinterpret_cast<const void*>(bits + sizeof(std::uint32_t) * static_cast<std::uint32_t>(base >> 5)),
                         sizeof(word)))
        {
            continue;
        }
        for (std::int32_t i = base; word != 0 && i < numBits && i < base + 32; ++i)
        {
            const std::uint32_t bit = 1u << (i & 31);
            if ((word & bit) == 0)
            {
                continue;  // a free slot
            }
            word &= ~bit;
            if (!visit(data + static_cast<std::uintptr_t>(i) * elementSize))
            {
                return;
            }
        }
    }
}

}  // namespace lss::platform
