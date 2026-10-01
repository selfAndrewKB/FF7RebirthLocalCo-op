#include "platform/pool_bits.h"

#include <bit>

namespace lss::platform::binned_pools {

std::uint32_t CommittedFromLastRow(std::span<const std::uint64_t> lastRow, std::uint32_t desiredCapacity) noexcept
{
    std::uint32_t set = 0;
    for (const std::uint64_t word : lastRow)
    {
        set += static_cast<std::uint32_t>(std::popcount(word));
    }
    const std::uint32_t tail    = desiredCapacity % 64;
    const std::uint32_t padding = tail != 0 ? 64 - tail : 0;
    return set > padding ? set - padding : 0;
}

std::uint64_t LiveBlocks(std::span<const std::uint32_t> infos) noexcept
{
    std::uint64_t taken = 0;
    for (const std::uint32_t info : infos)
    {
        if ((info & 3u) == 1u)
        {
            taken += (info >> 2) & 0x7FFFu;
        }
    }
    return taken;
}

}  // namespace lss::platform::binned_pools
