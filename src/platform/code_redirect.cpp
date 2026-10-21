#include "platform/code_redirect.h"

#include "platform/code_memory.h"
#include "platform/x64_jump.h"

#include <cstring>

namespace lss::platform {

namespace {

/// Places `code` then a jump to `resume` near `site` (the shared code pool). Returns its address, or 0.
std::uintptr_t BuildCodePage(std::uintptr_t site, std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    return PlaceCodeNear(site, code.size() + kJumpLength,
                         [code, resume](std::uintptr_t at, std::span<std::uint8_t> placed) noexcept {
                             const auto back = EncodeJump(at + code.size(), resume);
                             if (!back)
                             {
                                 return false;
                             }
                             std::memcpy(placed.data(), code.data(), code.size());
                             std::memcpy(placed.data() + code.size(), back->data(), back->size());
                             return true;
                         });
}

}  // namespace

bool CodeRedirect::Install(std::uintptr_t site, std::span<const std::uint8_t> replaced,
                           std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    const std::uintptr_t block = site & ~(kCodeBlockSize - 1);
    if (patch_.IsApplied() || replaced.size() < kJumpLength || site - block + replaced.size() > kCodeBlockSize)
    {
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(site), replaced.data(), replaced.size()) != 0)
    {
        return false;  // checked before any code is placed (the patch checks again at its swap)
    }

    const std::uintptr_t page = BuildCodePage(site, code, resume);
    const auto           jump = page != 0 ? EncodeJump(site, page) : std::nullopt;
    if (!jump)
    {
        return false;  // placed code is left unused: never reached, never freed
    }

    CodeBlock redirected{};
    std::memcpy(redirected.data(), jump->data(), jump->size());
    std::memset(redirected.data() + jump->size(), 0x90, replaced.size() - jump->size());  // nop
    return patch_.Apply(site, replaced, std::span<const std::uint8_t>(redirected.data(), replaced.size()));
}

bool CodeRedirect::Restore() noexcept
{
    return patch_.Revert();
}

}  // namespace lss::platform
