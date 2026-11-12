#pragma once

// Combat rumble on a seat's own pad (research/RE-p2-rumble.md).
//
// Two combat paths never reach a seat's player (static IDA):
//   - battle hits rumble only when the party leader is involved: the gate BattleHitFx_ShouldPlay_LeaderInvolved tests
//     attacker or target against the party manager's leader type (+0x150). Hits by or on a seat's member fail it,
//     so nothing plays. When it passes the controller is the attacker's own, else the primary (+0x140): player 1's;
//   - the montages' force-feedback notifies (AnimNotify_EndForceFeedback, ...Advanced, ...Range, ...RangeAdvanced)
//     play only for the leader's mesh (the same leader test) and always on UWorld::GetFirstPlayerController.
// The levers:
//   - a hit with a seat's member as attacker or target runs the gate inside a LeaderScope / LeaderTypeScope of
//     that member (the game's own leader test answers for the seat), and its camera effect and its primary-
//     controller fallback inside scopes::primary_controller::Scope (the seat's camera manager and controller);
//   - a notify on a seat's member's mesh (or the seat's ridden pawn) runs inside the same leader scopes and the
//     primary scope, and its GetFirstPlayerController calls answer the seat's controller. That last part is a
//     documented exception: no game path picks a second controller there; the rule is the game's own one for
//     its owner-based sources (the character's own controller).
// Owner-based sources (guard, ATB gain, Materia ATB, field hits) already pick a seat's controller: unchanged.
// Logged: every hit and notify that involves any player's character, every force-feedback play with its source
// (from the return address) and the controller it lands on, and a status line every 10 s.
// Game thread.
//
// Per seat (players/seats.h): a hit or a notify is the seat's whose member (or, for a notify, whose controller's pawn)
// it involves; it runs in that seat's leader view and primary scope, and the hit's camera and fallback calls follow the
// seat the gate passed for (one hit seat at a time). The counts are kept per seat.

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::combat_rumble {

/// The battle-hit sites, the notify slots and the play logging (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out). The calls placed from the game thread are
/// restored only from it, else left in place (they pass through); the first tick after this places nothing.
void Remove() noexcept;

/// Once per frame before the world tick (the engine-tick callback): on the first call installs the notifies' eight
/// first-controller call sites (two cross a 16-byte block; the game thread is outside the notifies here), and writes the
/// status line.
void BeforeTick() noexcept;

}  // namespace lss::feedback::combat_rumble
