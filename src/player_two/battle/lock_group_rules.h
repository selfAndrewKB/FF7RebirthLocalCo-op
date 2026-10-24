#pragma once

// The lock groups' rules (player_two/battle/lock_group_coin.h) as pure pieces, tested on the host
// (tests/lock_group_rules_test.cpp).

#include "players/seats.h"

#include <array>
#include <cstddef>

namespace lss::player_two::lock_group_rules {

/// The seat the rebalance counts as the player: when player 1 holds no lock in the group, the first seat (in seat order)
/// whose member holds one, so the leader-first respread runs only when no human holds one. 0: none (the game's own
/// leader). `seatsHolding`: bit s set when seat s's member holds a lock.
constexpr int RebalanceSeat(unsigned seatsHolding, bool playerOneHoldsLock) noexcept
{
    if (playerOneHoldsLock)
    {
        return 0;
    }
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        if ((seatsHolding & (1u << seat)) != 0)
        {
            return seat;
        }
    }
    return 0;
}

/// The groups whose rebalance holds a seat's member now, for the line on a change. A group past the capacity is still
/// answered, only its changes go unrecorded.
template <std::size_t N>
class HeldGroups
{
public:
    /// Records whether `group`'s rebalance holds a seat's member now; true when that changed (and could be recorded).
    bool Note(const void* group, bool hold) noexcept
    {
        const void** free = nullptr;
        for (auto& held : held_)
        {
            if (held == group)
            {
                if (!hold)
                {
                    held = nullptr;
                }
                return !hold;
            }
            if (held == nullptr && free == nullptr)
            {
                free = &held;
            }
        }
        if (hold && free != nullptr)
        {
            *free = group;
            return true;
        }
        return false;
    }

    /// The groups recorded as held, in slot order (null: a free slot).
    const std::array<const void*, N>& Groups() const noexcept { return held_; }

    void Clear() noexcept { held_ = {}; }

private:
    std::array<const void*, N> held_{};
};

}  // namespace lss::player_two::lock_group_rules
