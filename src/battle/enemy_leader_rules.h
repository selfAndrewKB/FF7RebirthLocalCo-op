#pragma once

// Whom enemies follow while the game's leader is down in co-op (battle/enemy_leader.h), as a pure rule over what the
// game reports; tested on the host (tests/enemy_leader_rules_test.cpp).
//
// In single player the game's forced switch moves the leader to a standing member when it goes down. In co-op that
// switch is refused (player 1 stays on their downed character), so the member it would have taken stands in for it:
// the first other player's standing character, in seat order; else the next standing member in the battle list from
// the leader's place, wrapping around, that no player plays (the switch's own order); else nobody.

#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::battle::enemy_leader_rules {

/// One character as the rule sees it.
struct Character
{
    const void*  character = nullptr;
    std::uint8_t type      = 16;     // the player type (16: none)
    bool         standing  = false;  // the game's own "a valid target": not knocked out, not petrified
    bool         human     = false;  // a player plays it (the battle list's entries only)
};

/// The character standing in for the game's downed leader; a null character while the leader stands or nobody can.
/// `seats`: each other player's character in seat order (null when none). `battle`: the battle list in the game's order,
/// `leaderAt` the leader's place in it (negative: not found).
constexpr Character StandIn(bool leaderStands, std::span<const Character> seats, std::span<const Character> battle,
                            int leaderAt) noexcept
{
    if (leaderStands)
    {
        return {};
    }
    for (const Character& seat : seats)
    {
        if (seat.character != nullptr && seat.standing)
        {
            return seat;
        }
    }
    const int count = static_cast<int>(battle.size());
    if (leaderAt < 0 || leaderAt >= count)
    {
        return {};
    }
    for (int step = 1; step < count; ++step)
    {
        const Character& next = battle[static_cast<std::size_t>((leaderAt + step) % count)];
        if (next.character != nullptr && next.standing && !next.human)
        {
            return next;
        }
    }
    return {};
}

/// Whether a change of the enemy-facing character tells the enemies the leader changed (the game's OnChangeLeader):
/// not when the game's own leader changed since the last look (its own switch told them), and not for a change back to
/// a leader that is down (every member down: nobody to turn to).
constexpr bool TellsEnemies(bool facingChanged, bool leaderSwitched, bool toStandIn, bool leaderStands) noexcept
{
    return facingChanged && !leaderSwitched && (toStandIn || leaderStands);
}

}  // namespace lss::battle::enemy_leader_rules
