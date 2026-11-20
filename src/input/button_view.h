#pragma once

// Which player a merged button belongs to (pure logic, tested on the host).
//
// Each action in Square's tables keeps the merged held byte and a mask of the sources holding it
// (research/RE-p2-per-pad-buttons.md; confirmed live: pad 1 is bit 0, pad 2 bit 1). A player's view of
// the action is "held, and held by one of that player's sources". A seat's sources are its own pad's
// (players/seats.h: seat s plays pad s, XInput bit s, PlayStation bit 4+s, DirectInput bit 21+s), and the keyboard's
// and the mouse's (bits 17-19) while the keyboard plays that seat; player 1 has every other source: pad 1, the
// keyboard and mouse beside it, the game's own synthetic keys, and the pad of any seat
// whose player is not in (as the game's one-pad merge has it). Requiring the merged held byte as well
// keeps the game's releases that leave the mask set (a press the UI consumed, the Tick's auto-released
// keys).
//
// The game reads "pressed" as previous == 0 and held == 1. Its previous byte comes from the merged
// state, so each player's previous view is kept here, one frame at a time. Every view is indexed by
// the seat (0 player 1).

#include "players/seats.h"

#include <cstdint>

namespace lss::input::button_view {

inline constexpr int kPlayers = players::kMaxPlayers;

/// The source bits of controller id `id`: its XInput pad, its PlayStation pad (the game tags a press under the
/// PlayStation reader's device scope with 4 + id) and its DirectInput pad.
constexpr std::uint32_t PadBits(int id) noexcept
{
    const int xinput      = id < 3 ? id : 3;
    const int playStation = 4 + (id < 3 ? id : 3);
    const int directInput = 21 + (id < 9 ? id : 9);
    return (1u << xinput) | (1u << playStation) | (1u << directInput);
}

/// The keyboard's sources: its two binding rows' bits and the mouse's (every mouse button, its repeat and the wheel).
inline constexpr std::uint32_t kKeyboardBits = (1u << 17) | (1u << 18) | (1u << 19);

/// The sources of `seat` (1.. a seat): its pad's, and the keyboard's while the keyboard plays it (`keyboardSeat`, the
/// seat the keyboard plays alone; 0: beside player 1).
constexpr std::uint32_t SeatSources(int seat, int keyboardSeat) noexcept
{
    return PadBits(seat) | (seat == keyboardSeat ? kKeyboardBits : 0u);
}

/// `seat` in a mask of seats (bit s for seat s).
constexpr unsigned SeatBit(int seat) noexcept
{
    return 1u << seat;
}

/// The sources of the seats in `seats` (a mask of SeatBit; player 1's bit is ignored).
constexpr std::uint32_t SeatPads(unsigned seats, int keyboardSeat = 0) noexcept
{
    std::uint32_t bits = 0;
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if ((seats & SeatBit(seat)) != 0)
        {
            bits |= SeatSources(seat, keyboardSeat);
        }
    }
    return bits;
}

/// The sources that belong to `player` (0 player 1, else a seat), with the seats in `seats` in: a seat's are its own
/// (SeatSources); player 1's are everything but the seats' in.
constexpr std::uint32_t PlayerSources(int player, unsigned seats, int keyboardSeat = 0) noexcept
{
    return player == 0 ? ~SeatPads(seats, keyboardSeat) : SeatSources(player, keyboardSeat);
}

/// The seat in `seats` whose device sent `sourceBit` (a source bit number, 0..31), or 0: player 1's source.
constexpr int SeatOfSource(std::uint32_t sourceBit, unsigned seats, int keyboardSeat = 0) noexcept
{
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if ((seats & SeatBit(seat)) != 0 && ((SeatSources(seat, keyboardSeat) >> (sourceBit & 31)) & 1) != 0)
        {
            return seat;
        }
    }
    return 0;
}

/// Whether `player` sees the action held, with the seats in `seats` in.
constexpr bool Held(std::uint8_t mergedHeld, std::uint32_t sources, int player, unsigned seats, int keyboardSeat = 0) noexcept
{
    return mergedHeld != 0 && (sources & PlayerSources(player, seats, keyboardSeat)) != 0;
}

/// One action's per-player held state, this frame and the one before, and the game's own merged
/// held state as it would be without the mod (this frame and the one before: the game's own previous
/// and held bytes under its one-pad merge).
///
/// The mod leaves a view in the held byte between frames (player 1's, or the game's merged state for
/// the menu openers and while a menu owns input), so the byte it reads back is not always the game's
/// merged state: with a seat's pad alone holding, it reads the 0 it wrote, and the game does not rewrite it
/// until the next fresh press or full release (its repeat update needs held == 1). So the game's own
/// held state is tracked here: released when no source holds the action; the byte's value when it
/// differs from what the mod left (the game wrote it this frame); otherwise unchanged. The one case
/// this cannot see is a game writer setting the byte to the same value the mod left (a press the UI
/// consumed while only a seat's pad held it): that seat's player then keeps seeing it held until the
/// release.
struct Views
{
    bool         now[kPlayers]      = {};
    bool         previous[kPlayers] = {};
    std::uint8_t gameHeld           = 0;
    std::uint8_t gamePrevious       = 0;  // the game's merged held state the frame before (its previous byte)
    std::uint8_t left               = 0;  // the byte the mod left for the game (player 1's view unless Wrote says)
    bool         wrote              = false;

    /// Once per frame, after the game's own advance. `byte` is the held byte as read now; `seats` the seats in;
    /// `keyboardSeat` the seat the keyboard plays alone (0: beside player 1).
    void Advance(std::uint8_t byte, std::uint32_t sources, unsigned seats, int keyboardSeat = 0) noexcept
    {
        gamePrevious = gameHeld;
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
            now[player]      = Held(gameHeld, sources, player, seats, keyboardSeat);
        }
        left  = now[0] ? 1 : 0;
        wrote = true;
    }

    /// The held byte the mod wrote this frame when it is not player 1's view (the game's merged state): what the
    /// next frame's Advance compares the byte with.
    void Wrote(std::uint8_t byte) noexcept
    {
        left = byte;
    }
};

}  // namespace lss::input::button_view
