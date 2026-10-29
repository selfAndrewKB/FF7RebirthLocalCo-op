#pragma once

// Choosing one pad's sticks from Square's per-pad records, as a pure function (tested on the host:
// tests/stick_select_test.cpp). input/pad_sticks.h explains where the records live.
//
// The game merges pads by magnitude, per stick; the same rule is applied here to the records of
// one pad only (a pad can have a record in more than one device bank). A merged stick that is
// exactly zero means the game's merge did not run (the window is not focused) or every pad is
// idle, and it stays zero.

#include <array>
#include <cstdint>
#include <span>

namespace lss::input {

/// One device bank: its records (six floats each: LX, LY, RX, RY, LT, RT) and how many there are.
struct StickBank
{
    const float* records = nullptr;
    std::int32_t count   = 0;
};

/// LX, LY, RX, RY.
using Sticks = std::array<float, 4>;

/// The sticks of pad `controllerId`, given the merged sticks the game computed and the banks the
/// game merges from. A pad with no record in any bank reads as idle.
Sticks SelectPadSticks(std::span<const StickBank> banks, std::int32_t controllerId, const Sticks& merged) noexcept;

}  // namespace lss::input
