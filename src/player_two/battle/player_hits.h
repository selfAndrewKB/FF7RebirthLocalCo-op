#pragma once

// The further players' hits, and the hits they take, count as the player's (RE-p2-enemy-target-bias.md §12-§14).
// Written for player 2 below; every seat's member is treated alike, each call about the one seat whose member it
// names (the seat found from the attacking or hit character).
//
// Every battle character takes damage through its battle-control component's damage slot (+0x380; 1.005 +0x388). The
// enemy's override first adds the enemy's stagger build-up and the attacker's on-hit ATB; then the shared base applies
// the scripted HP floors and fires the enemy's Blueprint events (OnPreTakeDamage, OnTakeDamage, HPFirst, OnGiveDamage,
// OnKilled, the damage phase's OnExtraCondition); then the override charges the summon gauge. Each step asks whether
// the attacker (or the victim) is the player: the leader test (IsUCPC_Core) everywhere but the summon gauge, which
// compares the attacker with the party leader pointer. Player 2's melee and projectile hits are applied in the damage
// source's tick and the battle system's tick, outside every player-2 scope, so the game answered "an AI ally" for
// them: no player-only reactions, the AI stagger rate, no on-hit ATB from normal attacks, the summon gauge's guest rate.
//
// For one damage call in which player 2's character is the attacker (any slot) or the victim (the battle-player slot),
// this holds the leader test's yes for player 2's member type on this thread; player 1 keeps the game's own yes. That
// is the single-player answer in each half: both players count. With player 2 as the attacker, the summon gauge's one
// leader read answers player 2's character. Nothing is written to the party manager: the leader pointer and type stay
// player 1's, so `GetPartyLeader` and the other pointer reads are unchanged (§14.1).
//
// The ten damage slots (every vtable holding one of the seven damage functions) are replaced. Game thread only: every
// root of the damage call found is the game thread (§14.3); a call on another thread passes straight through.
//
// The same answer is held when an enemy senses player 2's attack starting (OnReactionSensing and its TakeAbility
// variant, whose argument is the attacking character; §16.4): any thread, since enemy AI may tick on workers.
//
// Each damage or reaction call also records which human it is about (the attacker, or the character hit), the
// reaction context: player_two/target_coin answers an enemy's "who is the leader" with that player there (the user's
// rule, 2026-10-14: a counter aims at the player who hit it), and with the enemy's coin everywhere else.
// Logged: each call that held the answer and each summon-gauge answer (Debug), the install (Info).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::player_hits {

/// Replaces the damage slots and places the summon gauge's leader-read and the reaction-sensing redirects (logged).
/// Needs leader_answer's slot. Call from on_unreal_init: one reaction-sensing call crosses a 16-byte block
/// in both builds, which is safe to change only while no thread can be running it.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// No human: outside a damage or reaction call, or in one between enemies and AI members.
inline constexpr int kNoHuman = -1;

/// The human a damage or reaction call running on this thread is about: 0 player 1, s seat s's player, or kNoHuman.
int HumanInCall() noexcept;

}  // namespace lss::player_two::player_hits
