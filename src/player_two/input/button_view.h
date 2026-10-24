#pragma once

// Which player a merged button belongs to (pure logic, tested on the host).
//
// Each action in Square's tables keeps the merged held byte and a mask of the sources holding it
// (RE-p2-per-pad-buttons.md §1.2; confirmed live: pad 1 is bit 0, pad 2 bit 1). A player's view of
// the action is "held, and held by one of that player's sources". A seat's sources are its own pad's
// (players/seats.h: seat s plays pad s, XInput bit s and DirectInput bit 21+s); player 1 has every
// other source: pad 1, the keyboard, the mouse, the game's own synthetic keys, and the pad of any seat
// whose player is not in (as the game's one-pad merge has it). Requiring the merged held byte as well
// keeps the game's releases that leave the mask set (a press the UI consumed, the Tick's auto-released
// keys).
//
// The game reads "pressed" as previous == 0 and held == 1. Its previous byte comes from the merged
// state, so each player's previous view is kept here, one frame at a time. Every view is indexed by
// the seat (0 player 1).

#include "players/seats.h"

#include <cstdint>

namespace lss::player_two::button_view {

inline constexpr int kPlayers = players::kMaxPlayers;

/// The source bits of controller id `id` (XInput pad and DirectInput pad).
constexpr std::uint32_t PadBits(int id) noexcept
{
    const int xinput      = id < 3 ? id : 3;
    const int directInput = 21 + (id < 9 ? id : 9);
    return (1u << xinput) | (1u << directInput);
}

/// `seat` in a mask of seats (bit s for seat s).
constexpr unsigned SeatBit(int seat) noexcept
{
    return 1u << seat;
}

/// The pads of the seats in `seats` (a mask of SeatBit; player 1's bit is ignored).
constexpr std::uint32_t SeatPads(unsigned seats) noexcept
{
    std::uint32_t bits = 0;
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if ((seats & SeatBit(seat)) != 0)
        {
            bits |= PadBits(seat);
        }
    }
    return bits;
}

/// The sources that belong to `player` (0 player 1, else a seat), with the seats in `seats` in: a seat's are its own
/// pad's; player 1's are everything but the pads of the seats in.
constexpr std::uint32_t PlayerSources(int player, unsigned seats) noexcept
{
    return player == 0 ? ~SeatPads(seats) : PadBits(player);
}

/// The seat in `seats` whose pad sent `sourceBit` (a source bit number, 0..31), or 0: player 1's source.
constexpr int SeatOfSource(std::uint32_t sourceBit, unsigned seats) noexcept
{
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if ((seats & SeatBit(seat)) != 0 && ((PadBits(seat) >> (sourceBit & 31)) & 1) != 0)
        {
            return seat;
        }
    }
    return 0;
}

/// Whether `player` sees the action held, with the seats in `seats` in.
constexpr bool Held(std::uint8_t mergedHeld, std::uint32_t sources, int player, unsigned seats) noexcept
{
    return mergedHeld != 0 && (sources & PlayerSources(player, seats)) != 0;
}

/// One action's per-player held state, this frame and the one before, and the game's own merged
/// held state as it would be without the mod.
///
/// The mod leaves player 1's view in the held byte between frames, so the byte it reads back is not
/// the game's merged state: with pad 2 alone holding, it reads the 0 it wrote, and the game does not
/// rewrite it until the next fresh press or full release (its repeat update needs held == 1). So the
/// game's own held state is tracked here: released when no source holds the action; the byte's value
/// when it differs from what the mod left (the game wrote it this frame); otherwise unchanged.
/// The one case this cannot see is a game writer setting the byte to the same value the mod left
/// (a press the UI consumed while only a seat's pad held it): that seat's player then keeps seeing it
/// held until the release.
struct Views
{
    bool         now[kPlayers]      = {};
    bool         previous[kPlayers] = {};
    std::uint8_t gameHeld           = 0;
    std::uint8_t left               = 0;  // the byte the mod left for the game (player 1's view)
    bool         wrote              = false;

    /// Once per frame, after the game's own advance. `byte` is the held byte as read now; `seats` the seats in.
    void Advance(std::uint8_t byte, std::uint32_t sources, unsigned seats) noexcept
    {
        if (sources == 0)
        {
            gameHeld = 0;
        }
        else if (!wrote || byte != left)
        {
            gameHeld = byte;
        }
        for (int player = 0; player < kPlayers; ++player)
        {
            previous[player] = now[player];
            now[player]      = Held(gameHeld, sources, player, seats);
        }
        left  = now[0] ? 1 : 0;
        wrote = true;
    }
};

}  // namespace lss::player_two::button_view
