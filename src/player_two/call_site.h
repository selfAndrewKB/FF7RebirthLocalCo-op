#pragma once

// Checks for the build-specific paths that hear one caller of a game function some other way than a plain call
// redirect (RE-1005-field-2.md): at the function's entry, by the caller's return address, or with
// CallRedirect::InstallFromOwnThread. The call redirect checks the same before it writes (platform/call_redirect.h);
// an entry redirect does not see the call site, so the site is checked here before its return address is trusted.

#include "platform/guarded.h"

#include <cstdint>
#include <cstring>

namespace lss::player_two::call_site {

/// True if the five bytes at `site` (live) are `call rel32` (E8) resolving to `target` (live).
inline bool Calls(std::uintptr_t site, std::uintptr_t target) noexcept
{
    std::uint8_t bytes[5]{};
    if (site == 0 || target == 0 || !platform::CopyGuarded(bytes, reinterpret_cast<const void*>(site), sizeof(bytes)) ||
        bytes[0] != 0xE8)
    {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    return site + sizeof(bytes) + static_cast<std::intptr_t>(displacement) == target;
}

/// True if a call's five bytes at `site` lie inside one aligned 16-byte block (CallRedirect::Install takes it from
/// any thread; a crossing call needs InstallFromOwnThread).
inline bool FitsOneBlock(std::uintptr_t site) noexcept
{
    return site % 16 + 5 <= 16;
}

}  // namespace lss::player_two::call_site
