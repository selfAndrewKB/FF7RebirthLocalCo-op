#include "platform/call_redirect.h"

#include "platform/module_pin.h"

#include <cstring>
#include <limits>
#include <optional>

namespace lss::platform {

namespace {

constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::size_t  kCallLength = 5;

/// jmp qword ptr [rip+0] ; dq <address>
constexpr std::uint8_t kAbsoluteJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};

std::uintptr_t CallTarget(std::uintptr_t site, const std::uint8_t* bytes) noexcept
{
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return site + kCallLength + static_cast<std::intptr_t>(rel);
}

std::optional<std::int32_t> Displacement(std::uintptr_t site, std::uintptr_t to) noexcept
{
    const auto displacement = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(site + kCallLength);
    if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(displacement);
}

/// A sealed page near `site` holding a jump to `replacement`, or 0.
std::uintptr_t BuildThunk(std::uintptr_t site, std::uintptr_t replacement) noexcept
{
    const std::uintptr_t page = AllocateCodePageNear(site);
    if (page == 0)
    {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(page), kAbsoluteJump, sizeof(kAbsoluteJump));
    std::memcpy(reinterpret_cast<void*>(page + sizeof(kAbsoluteJump)), &replacement, sizeof(replacement));
    return SealCodePage(page) ? page : 0;
}

}  // namespace

bool CallRedirect::Install(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    const std::uintptr_t block = site & ~(kCodeBlockSize - 1);
    const std::size_t    at    = site - block;
    // The thunk jumps into our compiled code, so the DLL must outlive any chance of the call being
    // taken (platform/module_pin.h).
    if (block_ != 0 || replacement == 0 || at + kCallLength > kCodeBlockSize || !PinThisModule())
    {
        return false;
    }

    CodeBlock current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (current[at] != kCallOpcode || CallTarget(site, current.data() + at) != target)
    {
        return false;
    }

    const std::uintptr_t thunk = BuildThunk(site, replacement);
    const auto           rel   = thunk != 0 ? Displacement(site, thunk) : std::nullopt;
    if (!rel)
    {
        return false;  // a built thunk is left allocated and unused: never reached, never freed
    }

    CodeBlock redirected = current;
    std::memcpy(redirected.data() + at + 1, &*rel, sizeof(*rel));
    target_ = target;  // before the swap: the replacement may run the moment the call reaches it
    if (!SwapCodeBlock(block, current, redirected))
    {
        target_ = 0;
        return false;
    }
    block_      = block;
    original_   = current;
    redirected_ = redirected;
    return true;
}

bool CallRedirect::Restore() noexcept
{
    if (block_ == 0)
    {
        return true;
    }
    if (!SwapCodeBlock(block_, redirected_, original_))
    {
        return false;
    }
    block_ = 0;
    return true;
}

}  // namespace lss::platform
