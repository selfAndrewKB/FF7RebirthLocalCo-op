#pragma once

// The lock groups' rules (player_two/battle/lock_group_coin.h) as pure pieces, tested on the host
// (tests/lock_group_rules_test.cpp).

#include <array>
#include <cstddef>

namespace lss::player_two::lock_group_rules {

/// The rebalance counts player 2 as the player when player 2 holds a lock in the group and player 1 holds none, so the
/// leader-first respread runs only when neither human holds one.
constexpr bool RebalanceHoldsPlayerTwo(bool playerTwoHoldsLock, bool playerOneHoldsLock) noexcept
{
    return playerTwoHoldsLock && !playerOneHoldsLock;
}

/// The groups whose rebalance holds player 2 now, for the line on a change. A group past the capacity is still
/// answered, only its changes go unrecorded.
template <std::size_t N>
class HeldGroups
{
public:
    /// Records whether `group`'s rebalance holds player 2 now; true when that changed (and could be recorded).
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
