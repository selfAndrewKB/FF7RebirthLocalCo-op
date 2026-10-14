#pragma once

// Which player's character an enemy's Condition target selector counts as "the UCPC" (the user-controlled player
// character) for one select (RE-p2-enemy-target-bias.md §5.1, §10). Pure logic, tested on the host.
//
// In the single-player game the UCPC is the one character the player controls, and the story bosses' condition
// lists weight it 2:1 to 8:2 over the AI members. With two players each select names one of them, so both get the
// player's weight equally:
//   - player 2 not in (or playing no member): the game's own answer, untouched;
//   - both players' characters eligible (the game's own rule: a valid target and not excluded): a coin;
//   - only one eligible: that one, so a knocked-out or excluded player never takes the weight from the other;
//   - neither: player 1, the game's own answer.

#include <cstdint>

namespace lss::player_two::ucpc_choice {

enum class Ucpc : std::uint8_t
{
    Untouched,  // the game's own answer (player 2 is not in)
    PlayerOne,
    PlayerTwo,
};

enum class Why : std::uint8_t
{
    NoPlayerTwo,
    Coin,             // both eligible
    OnlyEligible,     // just this one eligible
    NeitherEligible,  // player 1, as the game would
};

struct Choice
{
    Ucpc ucpc = Ucpc::Untouched;
    Why  why  = Why::NoPlayerTwo;
};

/// `coin` is consulted only when both characters are eligible (true: player 2).
constexpr Choice Choose(bool playerTwoIn, bool oneEligible, bool twoEligible, bool coin) noexcept
{
    if (!playerTwoIn)
    {
        return {Ucpc::Untouched, Why::NoPlayerTwo};
    }
    if (oneEligible && twoEligible)
    {
        return {coin ? Ucpc::PlayerTwo : Ucpc::PlayerOne, Why::Coin};
    }
    if (oneEligible || twoEligible)
    {
        return {twoEligible ? Ucpc::PlayerTwo : Ucpc::PlayerOne, Why::OnlyEligible};
    }
    return {Ucpc::PlayerOne, Why::NeitherEligible};
}

}  // namespace lss::player_two::ucpc_choice
