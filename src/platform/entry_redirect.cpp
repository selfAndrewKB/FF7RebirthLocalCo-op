#include "platform/entry_redirect.h"

#include "platform/module_pin.h"
#include "platform/x64_jump.h"

#include <cstring>

namespace lss::platform {

namespace {

/// jmp qword ptr [rip+0] ; dq <address>
constexpr std::uint8_t kAbsoluteJump[]  = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
constexpr std::size_t  kTrampolineStart = 0x10;  // after the thunk's 14 bytes

/// A sealed page near `entry`: at +0 a jump to `replacement`; at +0x10 the trampoline (`first`, then
/// a jump to the instruction after it). Returns the page, or 0.
std::uintptr_t BuildPage(std::uintptr_t entry, std::span<const std::uint8_t> first, std::uintptr_t replacement) noexcept
{
    const std::uintptr_t page = AllocateCodePageNear(entry);
    if (page == 0)
    {
        return 0;
    }
    const std::uintptr_t trampoline = page + kTrampolineStart;
    const auto           back       = EncodeJump(trampoline + first.size(), entry + first.size());
    if (!back)
    {
        ReleaseCodePage(page);
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(page), kAbsoluteJump, sizeof(kAbsoluteJump));
    std::memcpy(reinterpret_cast<void*>(page + sizeof(kAbsoluteJump)), &replacement, sizeof(replacement));
    std::memcpy(reinterpret_cast<void*>(trampoline), first.data(), first.size());
    std::memcpy(reinterpret_cast<void*>(trampoline + first.size()), back->data(), back->size());
    return SealCodePage(page) ? page : 0;
}

}  // namespace

bool EntryRedirect::Install(std::uintptr_t entry, std::span<const std::uint8_t> first, std::uintptr_t replacement) noexcept
{
    const std::uintptr_t block = entry & ~(kCodeBlockSize - 1);
    const std::size_t    at    = entry - block;
    // The thunk jumps into our compiled code, so the DLL must outlive any chance of the function
    // being entered (platform/module_pin.h).
    if (block_ != 0 || replacement == 0 || first.size() < kJumpLength || at + first.size() > kCodeBlockSize ||
        !PinThisModule())
    {
        return false;
    }

    CodeBlock current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (std::memcmp(current.data() + at, first.data(), first.size()) != 0)
    {
        return false;
    }

    const std::uintptr_t page = BuildPage(entry, first, replacement);
    const auto           jump = page != 0 ? EncodeJump(entry, page) : std::nullopt;
    if (!jump)
    {
        return false;  // a built page is left allocated and unused: never reached, never freed
    }

    CodeBlock redirected = current;
    std::memcpy(redirected.data() + at, jump->data(), jump->size());
    std::memset(redirected.data() + at + jump->size(), 0x90, first.size() - jump->size());  // nop, never run
    trampoline_ = page + kTrampolineStart;  // before the swap: the replacement may run the moment it is reachable
    if (!SwapCodeBlock(block, current, redirected))
    {
        trampoline_ = 0;
        return false;
    }
    block_      = block;
    original_   = current;
    redirected_ = redirected;
    return true;
}

bool EntryRedirect::Restore() noexcept
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
