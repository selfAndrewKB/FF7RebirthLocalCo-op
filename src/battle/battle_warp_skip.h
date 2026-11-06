#pragma once

// Battles do not warp a seat's member (the user, 2026-10-12): the seat's player stays where they are when a
// battle starts or ends, and walks to the fight themselves (RE-p2-battle-gather-warp.md, S-BTLWARP and S-BTLWARP2).
//
// At battle start the game's PartyBattleAIMgr queues a fade-in warp next to the field leader for every battle
// member but the leader; at battle end it warps each member farther than 8 m from the leader (or not nav-reachable)
// back to it. Neither reads possession, so a seat's member is warped both times. The game's own skips are used:
//   - start: QueueBattleInTeleports' exclusion-set test (the one that skips player 1's vehicle riders and the
//     data's no-warp names) answers "excluded" for a seat's member; one call site redirected;
//   - end: QueueBattleFinishTeleports' per-member placement returns at once for a seat's member, as its own
//     early-outs do (no entry, no AI pause); one call site redirected.
// A far member still joins the battle: membership, the AI's in-battle flag and the enemy target lists come from other
// code, and escape tests only the field leader (RE §S-BTLWARP2).
// Logged (Info): each skip, with the seat's member's distance from player 1's character. Game thread only; any other
// thread, or no seat playing, passes through. 1.005: the start's call crosses a 16-byte block, so the sites are placed
// from the game thread. Per seat: every built seat's member is skipped alike (the seat found from the character the
// game hands the call).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_warp_skip {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::battle_warp_skip
