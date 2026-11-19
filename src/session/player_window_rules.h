#pragma once

// The pure rules of the player windows (session/player_window). Host-tested (tests/player_window_test.cpp).
//
// A pad is its player: slot N of the pad table is player N's. Each player has one window, on their own view, and two
// independent changes, each a swap:
//   - the d-pad left/right trades player numbers with the nearest player that way: the two pads trade slots and the two
//     screens trade numbers, so each person keeps their pad and their screen and takes the other number with its
//     character (player 1's: the game's leader);
//   - LB/RB moves the player's view one screen left/right, swapping places with what is there (another player's view or
//     a copy), so a person's own pad and their view end up in front of them;
//   - A, Start or a B tap closes it; B held a second takes player 2 or 3 out of the game (player 1 is refused).
// A pad on the title screen that presses a button becomes player 1's. A spare pad's Start takes back a player whose
// pad is gone before it adds a new player.

#include "input/pad_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace lss::session::player_window_rules {

namespace pad = input::pad_rules;

inline constexpr int kSeats = 3;  // player 1 and players 2 and 3: the pad slots 0..2, the screens 0..2

inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kLB        = 0x0100;  // XINPUT_GAMEPAD_LEFT_SHOULDER
inline constexpr std::uint16_t kRB        = 0x0200;  // XINPUT_GAMEPAD_RIGHT_SHOULDER

/// What a press in an open window asks for.
enum class Act : std::uint8_t
{
    Nothing,
    Trade,    // the player number: trade with the nearest player `dir` -1 lower or +1 higher
    Move,     // the view one screen `dir` -1 left or +1 right
    Close,
    Leave,    // B held: the player leaves the game
    Refused,  // B held on player 1's pad: player 1 never leaves
};

struct Action
{
    Act act = Act::Nothing;
    int dir = 0;

    constexpr bool operator==(const Action&) const noexcept = default;
};

/// One button's press (a down edge) on the pad of a player whose window is open. B is not a press here: its tap and
/// hold come at its let-go or after a second (OnB).
constexpr Action OnPress(std::uint16_t button) noexcept
{
    switch (button)
    {
        case kDpadLeft: return {Act::Trade, -1};
        case kDpadRight: return {Act::Trade, +1};
        case kLB: return {Act::Move, -1};
        case kRB: return {Act::Move, +1};
        case pad::kA:
        case pad::kStart: return {Act::Close};
        default: return {};
    }
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

using pad::WithPadOn;

/// The pad table with pad `which` playing player 1.
constexpr pad::PadTable WithPlayerOne(const pad::PadTable& table, int which) noexcept
{
    return WithPadOn(table, which, 0);
}

/// The keyboard takes player 1 on the title screen: the game's last device used turned to the keyboard there (a key, a
/// mouse button or the wheel), or the title came up with the keyboard the last device used (its keys skipped the
/// logos). `keyboard`/`title` now and at the last good read.
constexpr bool KeyboardClaims(bool keyboard, bool keyboardBefore, bool title, bool titleBefore) noexcept
{
    return title && keyboard && (!keyboardBefore || !titleBefore);
}

/// View held on pad `which` takes player 1 from the keyboard: the keyboard plays player 1, and the pad is on player 1's
/// slot or on no player's, and is no copy of another pad (`twin`: Steam Input's second read of a pad whose own View
/// hold is its player's window).
constexpr bool TakesPlayerOneFromKeyboard(const pad::PadTable& table, std::uint32_t playing, int which, bool twin) noexcept
{
    const int slot = pad::SlotOf(table, which);
    const bool someonesSlot = slot > 0 && slot < kSeats && pad::PlayerSlot(slot, playing);
    return pad::KeyboardPlaysOne(table) && !twin && slot >= 0 && !someonesSlot;
}

/// Players on slots `a` and `b` can trade numbers: each plays on a pad (the keyboard's player 1 stays player 1).
constexpr bool CanTradePads(const pad::PadTable& table, int a, int b) noexcept
{
    return pad::PadOn(table, a) != pad::kNoPad && pad::PadOn(table, b) != pad::kNoPad;
}

/// The pad table after the players on slots `a` and `b` trade numbers: each one's pad plays the other number, whatever
/// reader either pad is on. Unchanged unless CanTradePads.
constexpr pad::PadTable WithPadsTraded(const pad::PadTable& table, int a, int b) noexcept
{
    if (!CanTradePads(table, a, b))
    {
        return table;
    }
    const int padA = pad::PadOn(table, a);
    const int padB = pad::PadOn(table, b);
    return WithPadOn(WithPadOn(table, padA, b), padB, a);
}

/// `seat` is in while `playing` (bit N: seat N in; player 1 always).
constexpr bool Plays(std::uint32_t playing, int seat) noexcept
{
    return seat < kSeats && pad::PlayerSlot(seat, playing);
}

/// The seat pad `which` plays (its slot, while a player holds it; slot 0 always), or -1 for a pad that plays no one: a
/// pad on no player's slot, or muted there (`present`, bit p: pad p answers; pad_rules::Muted).
constexpr int SeatOf(const pad::PadTable& table, std::uint32_t playing, std::uint32_t present, int which) noexcept
{
    const int slot = pad::SlotOf(table, which);
    if (slot < 0 || pad::Muted(table, playing, present, which) || pad::PadOn(table, slot) == pad::kNoPad)
    {
        return -1;
    }
    if (slot == 0)
    {
        return 0;
    }
    return Plays(playing, slot) ? slot : -1;
}

/// The player `seat` trades numbers with in `dir` (-1 lower, +1 higher): the nearest number that way a player holds,
/// else -1 (the end of the numbers).
constexpr int TradePartner(int seat, int dir, std::uint32_t playing) noexcept
{
    for (int other = seat + dir; dir != 0 && other >= 0 && other < kSeats; other += dir)
    {
        if (Plays(playing, other))
        {
            return other;
        }
    }
    return -1;
}

/// Each screen's seat after players `a` and `b` trade numbers: each person keeps their screen, so the screen that showed
/// `a` shows `b` and the other way round; a copy's screen (-1) stays.
constexpr std::array<int, kSeats> Traded(std::array<int, kSeats> seatOn, int a, int b) noexcept
{
    for (std::size_t s = 0; s < seatOn.size(); ++s)
    {
        seatOn[s] = seatOn[s] == a ? b : seatOn[s] == b ? a : seatOn[s];
    }
    return seatOn;
}

/// The lowest player whose pad is gone (`gonePads`, bit p: pad p was connected and is not now), the one a spare pad's
/// Start takes back, else -1.
constexpr int SeatWithPadGone(const pad::PadTable& table, std::uint32_t playing, std::uint32_t gonePads) noexcept
{
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const int on = pad::PadOn(table, seat);  // kNoPad: the keyboard's, never gone
        if (Plays(playing, seat) && on >= 0 && (gonePads & (1u << on)) != 0)
        {
            return seat;
        }
    }
    return -1;
}

/// Each screen's seat after `seat` moves one screen `dir` among `screens`: swapped with what is there (another player,
/// or -1 for a copy's screen). Empty at the row's edge, with one screen, or for a seat on no screen.
constexpr std::optional<std::array<int, kSeats>> Moved(std::array<int, kSeats> seatOn, int screens, int seat, int dir) noexcept
{
    screens = screens < 1 ? 1 : screens > kSeats ? kSeats : screens;
    for (int s = 0; s < screens; ++s)
    {
        if (seatOn[static_cast<std::size_t>(s)] != seat)
        {
            continue;
        }
        const int to = s + dir;
        if (to < 0 || to >= screens || dir == 0)
        {
            return std::nullopt;
        }
        seatOn[static_cast<std::size_t>(s)]  = seatOn[static_cast<std::size_t>(to)];
        seatOn[static_cast<std::size_t>(to)] = seat;
        return seatOn;
    }
    return std::nullopt;
}

/// The settings file's word for a screen order ("213"), from its players left to right.
inline std::wstring OrderWord(const std::array<int, kSeats>& players)
{
    std::wstring word;
    for (const int player : players)
    {
        word += static_cast<wchar_t>(L'0' + player);
    }
    return word;
}

}  // namespace lss::session::player_window_rules
