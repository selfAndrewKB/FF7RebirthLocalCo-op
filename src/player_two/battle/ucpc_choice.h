#pragma once

// Which player's character an enemy's Condition target selector counts as "the UCPC" (the user-controlled player
// character) for one select (RE-p2-enemy-target-bias.md §5.1, §10). Pure logic, tested on the host.
//
// In the single-player game the UCPC is the one character the player controls, and the story bosses' condition
// lists weight it 2:1 to 8:2 over the AI members. With further players each select names one of them, so every
// player gets the player's weight equally:
//   - no seat in (or none playing a member): the game's own answer, untouched;
//   - two or more players' characters eligible (the game's own rule: a valid target and not excluded): a uniform
//     pick among them, by a coin;
//   - only one eligible: that one, so a knocked-out or excluded player never takes the weight from the others;
//   - none: player 1, the game's own answer.
//
// The players are numbered as the seats are (players/seats.h): 0 is player 1, seat s is player s+1. The coin is a
// uniform 64-bit value, read only when two or more are eligible: the pick is the high word of coin x count, so with
// two eligible it is the coin's top bit (player 1 below 2^63, the other above), the two-player coin of before.

#include "players/seats.h"

#include <cstdint>

namespace lss::player_two::ucpc_choice {

/// A set of players: bit p is player p (bit 0 player 1, bit s seat s).
using Players = std::uint8_t;
static_assert(players::kMaxPlayers <= 8, "a player set fits in a byte");

/// No player chosen: the game's own answer.
inline constexpr int kUntouched = -1;

enum class Ucpc : std::uint8_t
{
    Untouched,  // the game's own answer (no seat is in)
    PlayerOne,
    PlayerTwo,  // a seat's character (player 2's while one seat is built; the dev census counts it so)
};

enum class Why : std::uint8_t
{
    NoPlayerTwo,      // no seat in
    Coin,             // two or more eligible
    OnlyEligible,     // just this one eligible
    NeitherEligible,  // none eligible: player 1, as the game would
};

struct Choice
{
    Ucpc ucpc   = Ucpc::Untouched;
    Why  why    = Why::NoPlayerTwo;
    int  player = kUntouched;  // 0 player 1, s seat s; kUntouched with Ucpc::Untouched
};

constexpr Players Bit(int player) noexcept
{
    return static_cast<Players>(1u << player);
}

constexpr int Count(Players players) noexcept
{
    int count = 0;
    for (; players != 0; players = static_cast<Players>(players & (players - 1)))
    {
        ++count;
    }
    return count;
}

/// The `n`th player of `players` in player order (n from 0), or kUntouched past the last.
constexpr int Nth(Players players, int n) noexcept
{
    for (int player = 0; player < 8; ++player)
    {
        if ((players & Bit(player)) != 0 && n-- == 0)
        {
            return player;
        }
    }
    return kUntouched;
}

/// A uniform index in [0, count) from a uniform 64-bit coin: the high word of coin x count (count 2: the top bit).
constexpr int UniformIndex(std::uint64_t coin, int count) noexcept
{
    const auto n    = static_cast<std::uint64_t>(count);
    const auto high = (coin >> 32) * n + (((coin & 0xFFFFFFFFull) * n) >> 32);
    return static_cast<int>(high >> 32);
}

/// The players among `in` (bit 0, player 1, is always counted in) whose characters are `eligible`.
constexpr Players Candidates(Players in, Players eligible) noexcept
{
    return static_cast<Players>(eligible & (in | Bit(0)));
}

/// Whether Choose reads its coin for these sets: a seat is in and two or more are eligible. A caller draws the coin
/// only then, so the coin stream advances exactly as often as before.
constexpr bool NeedsCoin(Players in, Players eligible) noexcept
{
    return (in & ~Bit(0)) != 0 && Count(Candidates(in, eligible)) >= 2;
}

/// `in`: player 1 and the seats whose players play a member in this fight. `eligible`: whose characters pass the
/// game's rule for this select. `coin` is read only when NeedsCoin.
constexpr Choice Choose(Players in, Players eligible, std::uint64_t coin) noexcept
{
    if ((in & ~Bit(0)) == 0)
    {
        return {Ucpc::Untouched, Why::NoPlayerTwo, kUntouched};
    }
    const Players candidates = Candidates(in, eligible);
    const int     count      = Count(candidates);
    int           player     = 0;  // none eligible: player 1, as the game would
    if (count >= 2)
    {
        player = Nth(candidates, UniformIndex(coin, count));
    }
    else if (count == 1)
    {
        player = Nth(candidates, 0);
    }
    const Why why = count >= 2 ? Why::Coin : count == 1 ? Why::OnlyEligible : Why::NeitherEligible;
    return {player == 0 ? Ucpc::PlayerOne : Ucpc::PlayerTwo, why, player};
}

}  // namespace lss::player_two::ucpc_choice
