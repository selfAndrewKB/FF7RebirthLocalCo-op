#pragma once

// Encoding of the x64 jumps the mod writes: `jmp rel32` into game code (E9 + a signed 32-bit
// displacement measured from the end of the 5-byte instruction), the rel32 itself (a call's
// displacement is the same rule), and the absolute `jmp [rip+0]; dq address` of the thunks placed
// near the game (call_redirect.h, entry_redirect.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace lss::platform {

inline constexpr std::size_t kJumpLength         = 5;
inline constexpr std::size_t kAbsoluteJumpLength = 14;

/// The displacement of a rel32 instruction ending at `next` that reaches `to`, or empty if `to` is out
/// of its ±2 GiB reach.
constexpr std::optional<std::int32_t> Rel32(std::uintptr_t next, std::uintptr_t to) noexcept
{
    const auto displacement = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(next);
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(displacement);
}

/// The bytes of a `jmp` placed at `from` that lands on `to`, or empty if `to` is out of the
/// instruction's ±2 GiB reach.
constexpr std::optional<std::array<std::uint8_t, kJumpLength>> EncodeJump(std::uintptr_t from, std::uintptr_t to) noexcept
{
    const auto displacement = Rel32(from + kJumpLength, to);
    if (!displacement)
    {
        return std::nullopt;
    }
    const auto rel = static_cast<std::uint32_t>(*displacement);
    return std::array<std::uint8_t, kJumpLength>{
        0xE9,
        static_cast<std::uint8_t>(rel),
        static_cast<std::uint8_t>(rel >> 8),
        static_cast<std::uint8_t>(rel >> 16),
        static_cast<std::uint8_t>(rel >> 24),
    };
}

/// The bytes of `jmp qword ptr [rip+0]; dq to`: a jump that reaches anywhere, for code placed near the
/// game that jumps into this DLL. It touches neither the stack nor a register.
constexpr std::array<std::uint8_t, kAbsoluteJumpLength> EncodeAbsoluteJump(std::uintptr_t to) noexcept
{
    std::array<std::uint8_t, kAbsoluteJumpLength> bytes{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    for (std::size_t i = 0; i < sizeof(to); ++i)
    {
        bytes[6 + i] = static_cast<std::uint8_t>(to >> (8 * i));
    }
    return bytes;
}

}  // namespace lss::platform
