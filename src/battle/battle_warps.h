#pragma once

// Which party members the game warps at a battle's start and end (research/RE-p2-battle-gather-warp.md).
//
// At battle start the game's PartyBattleAIMgr queues a fade-in warp next to the field leader for every battle
// member but the leader; at battle end it warps each member farther than 8 m from the leader (or not nav-reachable)
// back to it. Neither reads who controls the member, so the character of player 2 or 3 is pulled away from where its
// player walked it. The game's own skips are used:
//   - start: QueueBattleInTeleports' exclusion-set test (the one that skips player 1's vehicle riders and the
//     data's no-warp names) answers "excluded" for a member that stays; one call site redirected;
//   - end: QueueBattleFinishTeleports' per-member placement returns at once for a member that stays, as its own
//     early-outs do (no entry, no AI pause); one call site redirected.
// The setting (LocalSplitScreen.ini [Battle] battle-warps, the console's `battle-warps`, the Co-op page's "Gather at
// Battle Start/End"):
//   on   the game's warps for everyone;
//   off  players 2 and 3's characters stay where they are; the game warps the members no player controls (the
//        default).
// A member stays in the battle wherever it is: membership, the AI's in-battle flag and the enemy target lists come
// from other code, and escape tests only the field leader (research/RE-p2-battle-gather-warp.md).
// Logged: the setting (Info); each member the start or end keeps (Info, with its distance from player 1's
// character); each member it leaves to the game (Debug). Game thread only; any other thread passes through. 1.005: the start's call crosses a 16-byte block, so
// the sites are placed from the game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_warps {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Whether this battle's start reached the game's own gather (its data allows one) with `character` among the members
/// it would gather (not kept out by the data's own exclusion set), whatever the setting then answered. False before any
/// gather and after ForgetGather. Game thread.
bool GatheredFor(const void* character) noexcept;

/// The battle is over: its gather is forgotten. Game thread.
void ForgetGather() noexcept;

}  // namespace lss::battle::battle_warps
