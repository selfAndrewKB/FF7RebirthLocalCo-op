#include "platform/block_patch.h"

#include "platform/log.h"

#include <array>
#include <cstring>

namespace lss::platform {

bool BlockPatch::Apply(std::uintptr_t site, std::span<const std::uint8_t> expected, std::span<const std::uint8_t> desired,
                       bool crossingAllowed) noexcept
{
    const std::uintptr_t block   = site & ~(kCodeBlockSize - 1);
    const std::size_t    at      = site - block;
    const bool           crosses = at + expected.size() > kCodeBlockSize;
    if (applied_ || expected.empty() || expected.size() != desired.size() || expected.size() > kCodeBlockSize ||
        (crosses && !crossingAllowed))
    {
        return false;
    }

    // The bytes, read as one run over both blocks when the change crosses.
    std::array<std::uint8_t, 2 * kCodeBlockSize> current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), crosses ? current.size() : kCodeBlockSize);
    if (std::memcmp(current.data() + at, expected.data(), expected.size()) != 0)
    {
        return false;
    }
    auto changed = current;
    std::memcpy(changed.data() + at, desired.data(), desired.size());
    for (std::size_t i = 0; i < 2; ++i)
    {
        std::memcpy(original_[i].data(), current.data() + i * kCodeBlockSize, kCodeBlockSize);
        std::memcpy(patched_[i].data(), changed.data() + i * kCodeBlockSize, kCodeBlockSize);
    }
    block_   = block;
    crosses_ = crosses;
    if (!Swap(original_, patched_))
    {
        block_   = 0;
        crosses_ = false;
        return false;
    }
    applied_ = true;
    return true;
}

bool BlockPatch::Revert() noexcept
{
    if (!applied_)
    {
        return true;
    }
    if (!Swap(patched_, original_))
    {
        return false;
    }
    applied_ = false;
    return true;
}

bool BlockPatch::Reapply() noexcept
{
    if (block_ == 0 || applied_ || !Swap(original_, patched_))
    {
        return false;
    }
    applied_ = true;
    return true;
}

bool BlockPatch::Swap(const CodeBlock (&from)[2], const CodeBlock (&to)[2]) noexcept
{
    if (!SwapCodeBlock(block_, from[0], to[0]))
    {
        return false;
    }
    if (crosses_ && !SwapCodeBlock(block_ + kCodeBlockSize, from[1], to[1]))
    {
        // Keep the bytes whole: the first block back as it was. This fails only if another writer changed it since
        // the swap just made on this thread; the log says so.
        if (!SwapCodeBlock(block_, to[0], from[0]))
        {
            Log(L"FAILED: the code in the block at {:#x} is torn: its second block refused a swap and the first could not "
                L"be put back",
                block_);
        }
        return false;
    }
    return true;
}

}  // namespace lss::platform
