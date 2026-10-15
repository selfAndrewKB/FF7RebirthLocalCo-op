#pragma once

// Lock groups prefer player 1 or player 2 by each picking enemy's coin (RE-p2-enemy-target-bias.md §18, §20).
//
// A lock group (RegisterTargetLockGroup; in Rebirth only the Temple of the Ancients Stun Ray) spreads its enemies over
// the party, at most N on each target, and with bPrioritizeUCPC prefers "the player": Pick takes the party leader when
// the leader holds fewer locks than every other target, and the rebalance releases every lock and picks again,
// leader first, when no target holds more than one and the leader holds none. Both read the leader pointer, so in
// co-op the group prefers player 1 only. The user's rule (2026-10-15, "one on a human"): each pick prefers player 1 or
// player 2 by the picking enemy's coin, and the rebalance respreads only when neither human holds a lock.
//
// The levers: Pick's leader read and CountLocks' leader read (whose "is the leader" byte only the rebalance uses)
// answer a thread-local character while one is held; a pick whose coin names player 2 holds player 2's member for one
// call of Pick (and if player 2 cannot take it, Pick runs again with the game's own player-1 term); the rebalance
// holds player 2's member when player 2 holds a lock in that group and player 1 holds none. Pick's two call sites
// belong to target_census, whose wrapper calls Pick() here. Nothing is written to game state.
//
// Any thread for picks (the first pick runs in the enemy blueprint's OnStartBattle, thread unknown); the rebalance runs
// on the game thread. Logged: the install (Info), each pick and each change of the rebalance answer (Debug), each
// rebalance answer (Trace).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::lock_group_coin {

/// The game's Pick(group*, FWeakObjectPtr* out) and two unused integer registers, passed on.
using PickFn = std::uint64_t (*)(std::uint64_t group, std::uint64_t out, std::uint64_t c, std::uint64_t d);

/// Installs the leader-read and rebalance redirects (logged). Needs target_coin. At on_unreal_init.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// One lock-group pick through the game's `pick`, with the picking enemy's coin naming "the player". `first`: the
/// registration's first pick (else the battle tick's re-pick), for the log. Off or without player 2: `pick` once.
std::uint64_t Pick(PickFn pick, std::uint64_t group, std::uint64_t out, std::uint64_t c, std::uint64_t d,
                   bool first) noexcept;

}  // namespace lss::player_two::lock_group_coin
