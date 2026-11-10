#pragma once

// The battle switch's rules in co-op (session/party_switch.h), as pure functions over what the game reports; tested on
// the host (tests/party_switch_rules_test.cpp).

namespace lss::session::party_switch_rules {

/// Whether the game's switch running for `passSeat`'s player (0: player 1, else the seat whose command pass is in
/// flight) must pass over `character`: another player's member always (`seatPlaying` names the seat that plays it, 0
/// for none); in a seat's pass player 1's character too (`playerOnes`). A player's switch then takes only a free AI
/// member, and the seat's own member counts as the game counts the leader.
constexpr bool SwitchRefuses(int passSeat, int seatPlaying, bool playerOnes) noexcept
{
    if (seatPlaying != 0 && seatPlaying != passSeat)
    {
        return true;
    }
    return passSeat != 0 && playerOnes;
}

/// Whether the game's cycle keeps the gate its request puts on it (CanSwitch: more than one member standing, the
/// leader counted). In co-op a leader that cannot act (down or petrified) drops it: the game's own switch for such a
/// leader runs the cycle without that gate, and the mod refuses that forced switch, so the player's own press runs it.
constexpr bool RequiresCanSwitch(bool gameRequires, bool coop, bool leaderCannotAct) noexcept
{
    return gameRequires && !(coop && leaderCannotAct);
}

}  // namespace lss::session::party_switch_rules
