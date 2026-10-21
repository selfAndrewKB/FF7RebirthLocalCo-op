#pragma once

// RIP-relative addressing, as the mod's checks of the game's instructions compute it: an instruction's operand is
// reached from the instruction's end (its start plus its length) plus the signed 32-bit displacement it carries.
// Tested on the host (tests/rip_relative_test.cpp).

#include <cstddef>
#include <cstdint>

namespace lss::platform {

/// The address a RIP-relative instruction at `instruction`, `length` bytes long, addresses with `displacement`.
constexpr std::uintptr_t RipTarget(std::uintptr_t instruction, std::size_t length, std::int32_t displacement) noexcept
{
    return instruction + length + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
}

/// A displacement whose low byte may be raised by `step` with one byte written (no carry into the next byte).
constexpr bool LowByteFits(std::int32_t displacement, std::uint8_t step) noexcept
{
    return (displacement & 0xFF) <= 0xFF - step;
}

}  // namespace lss::platform
