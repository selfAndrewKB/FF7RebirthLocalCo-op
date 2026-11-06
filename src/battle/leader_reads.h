#pragma once

// The game's own native reads of "the party leader" made for an enemy follow that enemy's coin
// (RE-p2-leader-native-reads.md: S-LEADEREDGE, S-LEADERLEVERS and the user's decisions of 2026-10-15).
//
// Enemy blueprints ask "who is the player" through natives that battle/target_coin answers; this covers the places
// where the game's C++ reads the leader pointer itself, each for one enemy (player 1 everywhere in co-op until now):
//   - E1-E3 enemy combinations (the Turks at the Temple of the Ancients): the joint attack's target is the initiator's
//     coin, and the partner takes the same coin, so both aim at one player;
//   - E4 a boss's timed damage-phase reaction and E5 the damage-phase gate a boss re-runs after a party summon's cut
//     scene: the leader is the reaction's source; the boss's coin;
//   - E6 the reaction player-type filter (counters, avoids and guards that only "the player's" attacks provoke, 23
//     definitions): every human counts, so a seat's attacks provoke them too;
//   - E7 Reno's mines without a target head for the leader type's character: each mine's own coin;
//   - E8 Crimson Mare's rocket-punch arms: the boss's coin.
//
// The levers, all from the S-LEADERLEVERS table (both builds): a wrapper where the enemy is known records it in a
// thread-local around the game's own call (a call redirect, an entry redirect or a vtable slot); a redirect of the leader
// read inside answers a seat's member (or, for E7, that member's type) while that enemy's coin names the seat, else the
// game's own answer. E6's read answers the causer itself when it is a seat's member (the read is compared only for
// equality with the causer). Nothing is written to game state; no player-1 pointer is built by hand. Every seat
// playing a member is treated the same way; the seat comes from the enemy's coin or from the causing character.
//
// Any thread (enemy AI runs on workers too): every context is thread-local. Logged: the install per item (Info), each
// answer for the once-per-event items, the first answer per item with its thread (Debug), each E8 answer (Trace) and a
// change of an E8 arm answer (Debug).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::leader_reads {

/// Installs every item's levers (logged per item; an item with a lever that cannot be placed stays off, and whatever of
/// it was placed passes through). Needs target_coin. At on_unreal_init (the crossing calls' rule).
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

}  // namespace lss::battle::leader_reads
