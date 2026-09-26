#pragma once

// Player 2's battle actions: Attack, the unique ability, Guard and Dodge from pad 2 on player 2's own
// member (RE-p2-battle-input.md §5.1).
//
// Player 1's presses reach the game's one command controller (FieldCommandCtrl, EndUISystem+0xB8)
// through its UI handler stack; the controller's tick turns its request array into begin/hold/
// release calls on the party leader. Below that choice of character everything is per character:
// the member's move state and battle component. So player 2 gets their own small command layer,
// the controller's edge logic on pad 2's view, calling the game's own per-character functions on
// player 2's member:
//   Attack/unique: MoveState_BattleAttackBegin on the press (not while guarding), ...Hold while held
//   after that press, ...Release every frame it is not held; Guard: CharHolder1758_BattleGuardHold
//   every frame it is held, ...GuardEnd every frame it is not; Dodge: MoveState_BattleDodge_
//   QueuePending9 on the press (the member's own tick runs the queued roll the same frame).
// It runs inside player 2's member tick scopes (player_two/member_movement.h: pad 2's button view,
// player 2 as the primary controller, the leader test yes for the member), just before the tick.
// The battle phase gates are the controller's own (read only). Player 1's controller is never
// written: player 2's "guarding" and "hold started" are kept here.
//
// Each part is switched by the battle mask (1 dodge, 2 attack and unique, 4 guard). Dodge and attack
// are on at start (checked live); guard is off until it is. Every frame it also logs, on change,
// what it reads (the controller's gates and requests, both characters' battle preconditions and
// move-state bytes, pad 2's actions). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::battle_input {

bool Install(const platform::GameImage& image) noexcept;

/// Inside player 2's member tick scopes, before the tick: reads, logs on change, and acts per mask.
void InScopes(void* member, void* state) noexcept;

/// Diagnosis and staging: which parts act (1 dodge, 2 attack and unique, 4 guard). Logged.
void SetMask(unsigned mask) noexcept;

}  // namespace lss::player_two::battle_input
