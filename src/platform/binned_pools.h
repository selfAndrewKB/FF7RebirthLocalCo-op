#pragma once

// How full the engine allocator's small-block pools are (research/RE-oom-path.md, "Reading the pools live").
//
// The game's GMalloc is stock UE 4.26 FMallocBinned3. Each of its 74 small pools reserves a fixed 512 MB at
// startup; when one pool's reservation is used up, the next refill ends the game with "Ran out of memory
// allocating <block size + 1> bytes", whatever commit is free. The 32 KB pool (index 49) is the one that ran
// out. Nothing in the shipping build counts this, so the fill is read from the allocator's own tables:
// the live count of committed blocks-of-blocks (the last row of the pool's allocation bit tree) and the
// blocks in use (the Taken field of each block-of-blocks' info word). Reads are guarded copies without the
// allocator's lock: a reading can be a little stale, never unsafe (the tables are allocated once and never
// moved; pool memory itself is never read).

#include "platform/pool_bits.h"

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::platform::binned_pools {

inline constexpr std::uint32_t kPoolCount   = 74;
inline constexpr std::uint32_t kPool32K     = 49;  // serves 28673-32768 bytes
inline constexpr std::uint32_t kBlock32K    = 32768;
inline constexpr std::uint32_t kMaxRowWords = 128;  // 8192 blocks-of-blocks at most, one bit each

struct PoolFill
{
    std::uint32_t blockSize   = 0;
    std::uint32_t committed   = 0;  // blocks-of-blocks committed now
    std::uint32_t capacity    = 0;  // blocks-of-blocks the 512 MB holds
    std::uint32_t perBlock    = 0;  // blocks per block-of-blocks
    std::uint64_t liveBlocks  = 0;  // blocks handed out now
    bool          full        = false;  // the next refill ends the game
};

/// The game's Binned3 allocator, found and checked once (its vtable, and the 32 KB pool's block size).
class Pools
{
public:
    static std::optional<Pools> Locate(const GameImage& image) noexcept;

    /// One pool's fill, or empty if a read faulted or a value is out of range.
    std::optional<PoolFill> Read(std::uint32_t index) const noexcept;

private:
    explicit Pools(std::uintptr_t allocator) noexcept : allocator_(allocator) {}

    std::uintptr_t allocator_;
};

}  // namespace lss::platform::binned_pools
