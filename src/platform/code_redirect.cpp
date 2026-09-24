#include "platform/code_redirect.h"

#include "platform/x64_jump.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstring>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kBlockSize       = 16;
constexpr std::uintptr_t kAllocationGrain = 0x10000;     // VirtualAlloc reserves on 64 KiB boundaries
constexpr std::uintptr_t kSearchRange     = 0x7FF00000;  // within a rel32 jump's reach, with margin
constexpr std::size_t    kCodePageSize    = 0x1000;

/// Commits one read-write page within reach of `target` (both directions), or returns 0.
std::uintptr_t AllocateNear(std::uintptr_t target) noexcept
{
    const std::uintptr_t low  = target > kSearchRange ? target - kSearchRange : kAllocationGrain;
    const std::uintptr_t high = target + kSearchRange;
    const std::uintptr_t start = target & ~(kAllocationGrain - 1);

    for (std::uintptr_t distance = kAllocationGrain; distance < kSearchRange; distance += kAllocationGrain)
    {
        for (const bool below : {true, false})
        {
            if (below && distance > start)
            {
                continue;  // would wrap below address zero
            }
            const std::uintptr_t candidate = below ? start - distance : start + distance;
            if (candidate < low || candidate > high)
            {
                continue;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(candidate), &info, sizeof(info)) == 0 || info.State != MEM_FREE)
            {
                continue;
            }
            if (void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), kCodePageSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            {
                return reinterpret_cast<std::uintptr_t>(page);
            }
        }
    }
    return 0;
}

/// Writes `code` then a jump to `resume` into a page near `site`, and makes it executable.
/// Returns the page's address, or 0.
std::uintptr_t BuildCodePage(std::uintptr_t site, std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    if (code.size() + kJumpLength > kCodePageSize)
    {
        return 0;
    }
    const std::uintptr_t page = AllocateNear(site);
    if (page == 0)
    {
        return 0;
    }
    const auto back = EncodeJump(page + code.size(), resume);
    if (!back)
    {
        VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(page), code.data(), code.size());
    std::memcpy(reinterpret_cast<void*>(page + code.size()), back->data(), back->size());

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(page), kCodePageSize, PAGE_EXECUTE_READ, &previous))
    {
        VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(page), kCodePageSize);
    return page;
}

/// Replaces the aligned 16-byte block at `block` with `desired` if it still holds `expected`, in
/// one atomic store, lifting the page's protection only around it.
bool SwapBlock(std::uintptr_t block, const std::array<std::uint8_t, 16>& expected,
               const std::array<std::uint8_t, 16>& desired) noexcept
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(block), kBlockSize, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    LONG64 comparand[2];
    LONG64 exchange[2];
    std::memcpy(comparand, expected.data(), sizeof(comparand));
    std::memcpy(exchange, desired.data(), sizeof(exchange));
    const bool swapped =
        InterlockedCompareExchange128(reinterpret_cast<volatile LONG64*>(block), exchange[1], exchange[0], comparand) != 0;

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(block), kBlockSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), kBlockSize);
    return swapped;
}

}  // namespace


bool CodeRedirect::Install(std::uintptr_t site, std::span<const std::uint8_t> replaced,
                           std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    const std::uintptr_t block = site & ~(kBlockSize - 1);
    const std::size_t    at    = site - block;
    if (block_ != 0 || replaced.size() < kJumpLength || at + replaced.size() > kBlockSize)
    {
        return false;
    }

    Block current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (std::memcmp(current.data() + at, replaced.data(), replaced.size()) != 0)
    {
        return false;
    }

    const std::uintptr_t page = BuildCodePage(site, code, resume);
    const auto           jump = page != 0 ? EncodeJump(site, page) : std::nullopt;
    if (!jump)
    {
        return false;  // a built page is left allocated and unused: never reached, never freed
    }

    Block redirected = current;
    std::memcpy(redirected.data() + at, jump->data(), jump->size());
    std::memset(redirected.data() + at + jump->size(), 0x90, replaced.size() - jump->size());  // nop

    if (!SwapBlock(block, current, redirected))
    {
        return false;
    }
    block_      = block;
    original_   = current;
    redirected_ = redirected;
    return true;
}

bool CodeRedirect::Restore() noexcept
{
    if (block_ == 0)
    {
        return true;
    }
    if (!SwapBlock(block_, redirected_, original_))
    {
        return false;
    }
    block_ = 0;
    return true;
}

}  // namespace lss::platform
