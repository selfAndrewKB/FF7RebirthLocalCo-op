#pragma once

// The seats' hits, and the hits they take, count as the player's (research/RE-p2-enemy-target-bias.md).
// Every seat's member is treated alike, each call about the one seat whose member it
// names (the seat found from the attacking or hit character).
//
// Every battle character takes damage through its battle-control component's damage slot (+0x380; 1.005 +0x388). The
// enemy's override first adds the enemy's stagger build-up and the attacker's on-hit ATB; then the shared base applies
// the scripted HP floors and fires the enemy's Blueprint events (OnPreTakeDamage, OnTakeDamage, HPFirst, OnGiveDamage,
// OnKilled, the damage phase's OnExtraCondition); then the override charges the summon gauge. Each step asks whether
// the attacker (or the victim) is the player: the leader test (IsUCPC_Core) everywhere but the summon gauge, which
// compares the attacker with the party leader pointer. A seat's melee and projectile hits are applied in the damage
// source's tick and the battle system's tick, outside every seat's scope, so the game answered "an AI ally" for
// them: no player-only reactions, the AI stagger rate, no on-hit ATB from normal attacks, the summon gauge's guest rate.
//
// For one damage call in which a seat's character is the attacker (any slot) or the victim (the battle-player slot),
// this holds the leader test's yes for the seat's member type on this thread; player 1 keeps the game's own yes. That
// is the single-player answer in each half: every player counts. With the seat attacking, the summon gauge's one
// leader read answers the seat's character. Nothing is written to the party manager: the leader pointer and type stay
// player 1's, so `GetPartyLeader` and the other pointer reads are unchanged.
//
// The ten damage slots (every vtable holding one of the seven damage functions) are replaced. Game thread only: every
// root of the damage call found is the game thread; a call on another thread passes straight through.
//
// The same answer is held when an enemy senses a seat's attack starting (OnReactionSensing and its TakeAbility
// variant, whose argument is the attacking character): any thread, since enemy AI may tick on workers.
//
// And when a seat's character's voice notify tests whether its hit would kill the target
// (research/RE-voice-line-hit-test.md): the talk component runs the hit's damage as a dry run, then a lethal test whose one
// leader read decides whether the target's scripted HP floor stops the hit. Outside the damage slots that test judged
// the seat's hit as an AI ally's while the real hit counts as the player's, so the finishing-blow line could be lost
// on a target with a floor. The test's one call is redirected.
//
// Each damage or reaction call also records which human it is about (the attacker, or the character hit), the
// reaction context: battle/target_coin answers an enemy's "who is the leader" with that player there (a counter
// aims at the player who hit it), and with the enemy's coin everywhere else.
// Logged: each call that held the answer, each summon-gauge answer and each seat's lethal test with its result (Debug),
// the install (Info).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::player_hits {

/// Replaces the damage slots and places the summon gauge's leader-read, the reaction-sensing and the voice line's
/// lethal-hit test redirects (logged).
/// Needs leader_answer's slot. Call from on_unreal_init: one reaction-sensing call crosses a 16-byte block
/// in both builds, which is safe to change only while no thread can be running it.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// No human: outside a damage or reaction call, or in one between enemies and AI members.
inline constexpr int kNoHuman = -1;

/// The human a damage or reaction call running on this thread is about: 0 player 1, s seat s's player, or kNoHuman.
int HumanInCall() noexcept;

}  // namespace lss::battle::player_hits
