#pragma once

// The pure rules of the player windows (session/player_window). Host-tested (tests/player_window_test.cpp).
//
// A pad is its player: slot N of the pad table is player N's. Each player has one window, on their own view: A, Start
// or a B tap closes it; B held a second takes player 2 or 3 out of the game (player 1 is refused). A pad on the title
// screen that presses a button becomes player 1's.

#include "input/pad_rules.h"

#include <cstddef>
#include <cstdint>

namespace lss::session::player_window_rules {

namespace pad = input::pad_rules;

inline constexpr int kSeats = 3;  // player 1 and players 2 and 3: the pad slots 0..2

/// What a press in an open window asks for.
enum class Act : std::uint8_t
{
    Nothing,
    Close,
    Leave,    // B held: the player leaves the game
    Refused,  // B held on player 1's pad: player 1 never leaves
};

struct Action
{
    Act act = Act::Nothing;

    constexpr bool operator==(const Action&) const noexcept = default;
};

/// One button's press (a down edge) on the pad of a player whose window is open. B is not a press here: its tap and
/// hold come at its let-go or after a second (OnB).
constexpr Action OnPress(std::uint16_t button) noexcept
{
    return button == pad::kA || button == pad::kStart ? Action{Act::Close} : Action{};
}

/// B on the pad of `seat` with its window open: a tap closes the window, a hold leaves (player 1: refused).
constexpr Action OnB(int seat, bool tap, bool held) noexcept
{
    if (held)
    {
        return {seat == 0 ? Act::Refused : Act::Leave};
    }
    return tap ? Action{Act::Close} : Action{};
}

/// The pad table with `user`'s pad on player 1's slot (swapped with the pad there).
constexpr pad::PadTable WithPlayerOne(pad::PadTable table, int user) noexcept
{
    const int from = pad::SlotOf(table, user);
    if (from > 0)
    {
        table[static_cast<std::size_t>(from)] = table[0];
        table[0]                              = user;
    }
    return table;
}

/// The pad table with `user`'s pad on `slot` (swapped with the pad there).
constexpr pad::PadTable WithPadOn(pad::PadTable table, int user, int slot) noexcept
{
    const int from = pad::SlotOf(table, user);
    if (from >= 0 && slot >= 0 && slot < pad::kSlots && from != slot)
    {
        table[static_cast<std::size_t>(from)] = table[static_cast<std::size_t>(slot)];
        table[static_cast<std::size_t>(slot)] = user;
    }
    return table;
}

/// The seat `user`'s pad plays (its slot, while a player holds it; slot 0 always), or -1 for a pad that plays no one.
constexpr int SeatOf(const pad::PadTable& table, std::uint32_t playing, int user) noexcept
{
    const int slot = pad::SlotOf(table, user);
    if (slot == 0)
    {
        return 0;
    }
    return slot > 0 && slot < kSeats && (playing & (1u << slot)) != 0 ? slot : -1;
}

}  // namespace lss::session::player_window_rules
