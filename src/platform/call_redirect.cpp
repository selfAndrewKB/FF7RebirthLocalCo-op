#include "platform/call_redirect.h"

#include "platform/image.h"
#include "platform/module_pin.h"
#include "platform/x64_jump.h"

#include <array>
#include <cstring>
#include <optional>

namespace lss::platform {

namespace {

constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::uint8_t kJumpOpcode = 0xE9;  // jmp rel32: same length and displacement rule as the call
constexpr std::size_t  kCallLength = 5;

std::uintptr_t CallTarget(std::uintptr_t site, const std::uint8_t* bytes) noexcept
{
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return site + kCallLength + static_cast<std::intptr_t>(rel);
}

/// A jump to `replacement` placed near `site` (the shared code pool), or 0.
std::uintptr_t BuildThunk(std::uintptr_t site, std::uintptr_t replacement) noexcept
{
    return PlaceCodeNear(site, kAbsoluteJumpLength,
                         [replacement](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
                             const auto jump = EncodeAbsoluteJump(replacement);
                             std::memcpy(code.data(), jump.data(), jump.size());
                             return true;
                         });
}

}  // namespace

bool CallRedirect::Install(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, false, kCallOpcode);
}

bool CallRedirect::Install(const GameImage& image, std::uintptr_t idaSite, std::uintptr_t target,
                           std::uintptr_t replacement) noexcept
{
    const auto site = image.Rebase(idaSite, kCallLength);
    return site && Install(*site, target, replacement);
}

bool CallRedirect::InstallResearch(const GameImage& image, std::uintptr_t idaSite, std::uintptr_t idaTarget,
                                   std::uintptr_t replacement) noexcept
{
    const auto site   = image.Rebase(idaSite, kCallLength);
    const auto target = image.Rebase(idaTarget);
    return site && target && Install(*site, *target, replacement);
}

bool CallRedirect::InstallTailJump(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, false, kJumpOpcode);
}

bool CallRedirect::InstallFromOwnThread(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, true, kCallOpcode);
}

bool CallRedirect::Place(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement, bool crossingAllowed,
                         std::uint8_t opcode) noexcept
{
    const std::uintptr_t block   = site & ~(kCodeBlockSize - 1);
    const bool           crosses = site - block + kCallLength > kCodeBlockSize;
    // The thunk jumps into our compiled code, so the DLL must outlive any chance of the call being
    // taken (platform/module_pin.h).
    if (patch_.IsApplied() || replacement == 0 || (crosses && !crossingAllowed) || !PinThisModule())
    {
        return false;
    }

    std::array<std::uint8_t, kCallLength> current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(site), current.size());
    if (current[0] != opcode || CallTarget(site, current.data()) != target)
    {
        return false;
    }

    const std::uintptr_t thunk = BuildThunk(site, replacement);
    const auto           rel   = thunk != 0 ? Rel32(site + kCallLength, thunk) : std::nullopt;
    if (!rel)
    {
        return false;  // placed code is left unused: never reached, never freed
    }

    auto redirected = current;
    std::memcpy(redirected.data() + 1, &*rel, sizeof(*rel));
    target_ = target;  // before the swap: the replacement may run the moment the call reaches it
    if (!patch_.Apply(site, current, redirected, crossingAllowed))
    {
        target_ = 0;
        return false;
    }
    return true;
}

bool CallRedirect::Restore() noexcept
{
    return patch_.Revert();
}

}  // namespace lss::platform
