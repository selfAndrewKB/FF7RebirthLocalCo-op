#pragma once

// Lock groups prefer player 1 or a seat's player by each picking enemy's coin (research/RE-p2-enemy-target-bias.md).
//
// A lock group (RegisterTargetLockGroup; in Rebirth only the Temple of the Ancients Stun Ray) spreads its enemies over
// the party, at most N on each target, and with bPrioritizeUCPC prefers "the player": Pick takes the party leader when
// the leader holds fewer locks than every other target, and the rebalance releases every lock and picks again,
// leader first, when no target holds more than one and the leader holds none. Both read the leader pointer, so in
// co-op the group prefers player 1 only. The rule here is one lock on a human: each pick prefers player 1 or
// a seat's player by the picking enemy's coin, and the rebalance respreads only when no human holds a lock.
//
// The levers: Pick's leader read and CountLocks' leader read (whose "is the leader" byte only the rebalance uses)
// answer a thread-local character while one is held; a pick whose coin names a seat holds that seat's member for one
// call of Pick (and if its player cannot take it, Pick runs again with the game's own player-1 term); the rebalance
// holds a seat's member when that seat holds a lock in that group and player 1 holds none. Pick's two call sites (the
// battle tick's re-pick and the Blueprint registration's first pick) are redirected here; each pick is told to the
// one observer (battle/target_census, a dev tool). Nothing is written to game state.
//
// Any thread for picks (the first pick runs in the enemy blueprint's OnStartBattle, thread unknown); the rebalance runs
// on the game thread. Logged: the install (Info), each pick and each change of the rebalance answer (Debug), each
// rebalance answer (Trace).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_group_coin {

/// Installs the two pick calls, the leader-read and rebalance redirects (all or none; logged). Needs target_coin. At
/// on_unreal_init.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// What one lock-group pick reported: the group, the game's result (its low byte: picked), the out (an FWeakObjectPtr
/// of the target) and whether it was the registration's first pick. Any thread.
using PickObserver = void (*)(const void* group, std::uint64_t result, std::uint64_t out, bool first) noexcept;

/// The one observer of the picks (null: none). Set before any battle and cleared at unload.
void SetPickObserver(PickObserver observer) noexcept;

}  // namespace lss::battle::lock_group_coin
