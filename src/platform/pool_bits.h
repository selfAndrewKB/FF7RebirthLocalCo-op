#pragma once

// The counting behind platform/binned_pools.h, pure so it is tested on the host
// (tests/pool_bits_test.cpp). Layouts: research/RE-oom-path.md, "Reading the pools live".

#include <cstdint>
#include <span>

namespace lss::platform::binned_pools {

/// Committed blocks-of-blocks from the allocation tree's last row: its set bits, less the padding bits past
/// `desiredCapacity` that the allocator pre-sets to 1.
std::uint32_t CommittedFromLastRow(std::span<const std::uint64_t> lastRow, std::uint32_t desiredCapacity) noexcept;

/// Blocks in use from the per-block-of-blocks info words: the Taken field (bits 2-16) of each word whose canary
/// (bits 0-1) is 1, "assigned".
std::uint64_t LiveBlocks(std::span<const std::uint32_t> infos) noexcept;

}  // namespace lss::platform::binned_pools
