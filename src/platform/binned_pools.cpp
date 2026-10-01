#include "platform/binned_pools.h"

#include "platform/guarded.h"
#include "platform/image.h"

#include <array>
#include <span>

namespace lss::platform::binned_pools {

namespace {

// Facts read from the database (RE-oom-path.md, "Reading the pools live"; this build).
constexpr std::uintptr_t kAllocatorStatic = 0x149104AE8;  // FMallocBinned3::MallocBinned3, set by its constructor
constexpr std::uintptr_t kBinned3Vtable   = 0x145EB1AF8;
constexpr std::uint32_t  kPoolTables      = 0x28;  // FPoolTable[74]
constexpr std::uint32_t  kPoolStride      = 0x60;
// FPoolTable
constexpr std::uint32_t kBlockSize  = 0x00;  // u32
constexpr std::uint32_t kPerBlock   = 0x04;  // u16 blocks per block-of-blocks
constexpr std::uint32_t kAllocBits  = 0x08;  // FBitTree: bit 1 = block-of-blocks committed
constexpr std::uint32_t kEverUsed   = 0x48;  // u32, highest block-of-blocks ever used + 1
constexpr std::uint32_t kPoolInfos  = 0x50;  // u32**: page table; only page 0 is used (<= 8192 entries)
// FBitTree
constexpr std::uint32_t kBits      = 0x00;  // u64*
constexpr std::uint32_t kDesired   = 0x0C;  // u32, the pool's capacity in blocks-of-blocks
constexpr std::uint32_t kLastRow   = 0x14;  // u32, in qwords
constexpr std::uint32_t kAllocSize = 0x18;  // u32, bytes

constexpr std::uint32_t kMaxInfos = kMaxRowWords * 64;

template <class T>
bool ReadField(std::uintptr_t address, T& out) noexcept
{
    return address != 0 && CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

}  // namespace

std::optional<Pools> Pools::Locate(const GameImage& image) noexcept
{
    const auto slot   = image.Rebase(kAllocatorStatic, sizeof(std::uintptr_t));
    const auto vtable = image.Rebase(kBinned3Vtable);
    std::uintptr_t allocator = 0, actual = 0;
    std::uint32_t  blockSize = 0;
    if (!slot || !vtable || !ReadField(*slot, allocator) || !ReadField(allocator, actual) || actual != *vtable ||
        !ReadField(allocator + kPoolTables + kPool32K * kPoolStride + kBlockSize, blockSize) || blockSize != kBlock32K)
    {
        return std::nullopt;
    }
    return Pools(allocator);
}

std::optional<PoolFill> Pools::Read(std::uint32_t index) const noexcept
{
    if (index >= kPoolCount)
    {
        return std::nullopt;
    }
    const std::uintptr_t table = allocator_ + kPoolTables + index * kPoolStride;
    const std::uintptr_t tree  = table + kAllocBits;
    PoolFill             fill;
    std::uint16_t        perBlock = 0;
    std::uintptr_t       bits = 0, infosPage = 0, infos = 0;
    std::uint32_t        lastRow = 0, allocSize = 0, everUsed = 0;
    if (!ReadField(table + kBlockSize, fill.blockSize) || !ReadField(table + kPerBlock, perBlock) ||
        !ReadField(tree + kBits, bits) || !ReadField(tree + kDesired, fill.capacity) ||
        !ReadField(tree + kLastRow, lastRow) || !ReadField(tree + kAllocSize, allocSize) ||
        !ReadField(table + kEverUsed, everUsed) || !ReadField(table + kPoolInfos, infosPage))
    {
        return std::nullopt;
    }
    const std::uint32_t words = allocSize / 8 > lastRow ? allocSize / 8 - lastRow : 0;
    if (bits == 0 || words == 0 || words > kMaxRowWords || fill.capacity == 0 || fill.capacity > kMaxInfos)
    {
        return std::nullopt;
    }
    std::array<std::uint64_t, kMaxRowWords> row{};
    std::uint64_t                            root = 0;
    if (!CopyGuarded(row.data(), reinterpret_cast<const void*>(bits + lastRow * sizeof(std::uint64_t)),
                     words * sizeof(std::uint64_t)) ||
        !ReadField(bits, root))
    {
        return std::nullopt;
    }
    fill.committed = CommittedFromLastRow(std::span(row.data(), words), fill.capacity);
    fill.perBlock  = perBlock;
    fill.full      = root == ~0ull;
    if (infosPage != 0 && ReadField(infosPage, infos) && infos != 0)
    {
        std::array<std::uint32_t, kMaxInfos> words32;  // only the copied `count` words are read
        const std::uint32_t                  count = everUsed < kMaxInfos ? everUsed : kMaxInfos;
        if (count != 0 && CopyGuarded(words32.data(), reinterpret_cast<const void*>(infos), count * sizeof(std::uint32_t)))
        {
            fill.liveBlocks = LiveBlocks(std::span(words32.data(), count));
        }
    }
    return fill;
}

}  // namespace lss::platform::binned_pools
