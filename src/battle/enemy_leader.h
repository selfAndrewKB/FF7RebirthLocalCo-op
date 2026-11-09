#pragma once

// Enemies turn on a standing member when the game's leader is down in co-op (RE-enemy-leader-reads.md).
//
// In single player the game's forced switch moves the leader to a standing member when it is knocked out or
// petrified, and its leader change then fires every battle AI blueprint's OnChangeLeader (the Turks, Bottomswell and
// a few others re-aim there). In co-op that switch is refused (session/party_switch: player 1 stays on their downed
// character), so nothing changes for the enemies. The co-op result is the single-player one: enemies turn on the member
// the switch would have taken, a standing human's character first, else the AI ally (battle/enemy_leader_rules.h).
//   - The enemy-facing character is read each engine tick on the game thread while a battle runs and another player is
//     in. When it changes with no switch of the game's (the leader went down, the stand-in went down, the leader got
//     up), every enemy controller's OnChangeLeader runs with the character it followed before, through the game's own
//     walk: its snapshot of the battle's characters and its event call, the same functions its leader change calls.
//     The game's enemy target selectors already pass over a downed character (its own valid-target test), so no
//     target is set by hand.
//   - `Instead` publishes the stand-in for the reads that name "the player" for an enemy: battle/target_coin's natives,
//     battle/leader_reads' native reads and battle/equal_targeting's Condition selects answer it where they would have
//     answered player 1's downed character.
// The leader itself never moves. Logged on each change (Debug) with the number of enemies told; `enemy-leader` reads
// the state.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_leader {

bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, on the game thread: the enemy-facing character, and the enemies told when it changes.
void Tick() noexcept;

/// The standing member enemies follow while the game's downed leader stays (a human's, else the AI ally), and its
/// player type; null while the leader stands, nobody stands, or no other player is in. Any thread.
void* Instead(std::uint8_t* type = nullptr) noexcept;

#if LSS_DEV
/// The `enemy-leader` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::battle::enemy_leader
