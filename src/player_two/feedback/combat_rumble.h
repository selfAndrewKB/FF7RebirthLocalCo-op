#pragma once

// Player 2's combat rumble on player 2's pad (RE-p2-rumble.md §10; plan, "per-pad rumble: combat and other sources").
//
// Two combat paths never reach player 2 (static IDA, §10.4):
//   - battle hits rumble only when the party leader is involved: the gate BattleHitFx_ShouldPlay_LeaderInvolved tests
//     attacker or target against the party manager's leader type (+0x150). Hits by or on player 2's member fail it,
//     so nothing plays. When it passes the controller is the attacker's own, else the primary (+0x140): player 1's;
//   - the montages' force-feedback notifies (AnimNotify_EndForceFeedback, ...Advanced, ...Range, ...RangeAdvanced)
//     play only for the leader's mesh (the same leader test) and always on UWorld::GetFirstPlayerController.
// The levers:
//   - a hit with player 2's member as attacker or target runs the gate inside a LeaderScope / LeaderTypeScope of
//     player 2's member (the game's own leader test answers for player 2), and its camera effect and its primary-
//     controller fallback inside primary_controller::Scope (player 2's camera manager and controller);
//   - a notify on player 2's member's mesh (or player 2's ridden pawn) runs inside the same leader scopes and the
//     primary scope, and its GetFirstPlayerController calls answer player 2's controller. That last part is a
//     documented exception (§10.7): no game path picks a second controller there; the rule is the game's own one for
//     its owner-based sources (the character's own controller).
// Owner-based sources (guard, ATB gain, Materia ATB, field hits) already pick player 2's controller: unchanged.
// Logged: every hit and notify that involves either player's character, every force-feedback play with its source
// (from the return address) and the controller it lands on, and a status line every 10 s.
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::combat_rumble {

/// The battle-hit sites, the notify slots and the play logging (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with player 2 out). The calls placed from the game thread are
/// restored only from it, else left in place (they pass through); the first tick after this places nothing.
void Remove() noexcept;

/// Once per frame before the world tick (the engine-tick callback): on the first call installs the notifies' eight
/// first-controller call sites (two cross a 16-byte block; the game thread is outside the notifies here), and writes the
/// status line.
void BeforeTick() noexcept;

}  // namespace lss::player_two::combat_rumble
