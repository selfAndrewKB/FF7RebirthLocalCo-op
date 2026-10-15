#pragma once

// Enemies aim their moves at player 1 or player 2, one coin per enemy action (RE-p2-enemy-target-bias.md §12, §16).
//
// Enemy blueprints name "the player" with a few native functions: GetPartyLeader (the summons' signature lock-ons,
// distances and angles to the leader, lists that drop the leader), IsUCPC / IsAIPC (is this character the player, an
// AI member) and GetPartyLeaderType. In co-op these all name player 1, so every such move goes to player 1. The user's
// rule (2026-10-14): each enemy tosses a coin when it decides its next action and counts that player as "the player"
// until its next decision, so a whole attack follows one player and checks made every frame do not flicker. Inside a
// damage or reaction call the player it is about answers instead (a counter aims at the player who hit it;
// player_two/player_hits records which).
//
// The levers:
//   - the toss, exactly when one of the enemy's three decision events is about to run (before the blueprint chooses
//     and before the action's lock target is resolved), and only then: OnActionInvoke at its one call (the +0xAD8
//     slot's function skips it under a global gate or for interval type 4), OnActionInterruptInvoke and
//     OnActionTopPriorityInvoke at the +0xAE0 / +0xB18 slots of the two native enemy controller vtables, EnemyBase
//     and BossBase, under the events' own conditions (a non-null argument; BossBase's gates also mode 1, ctrl+0x101C);
//   - the UFunctions' native pointers (UFunction::Func, read by the blueprint VM on every call): GetPartyLeader and the
//     two controller aliases sharing its exec, IsUCPC, IsAIPC, GetPartyLeaderType. Each answers for a calling object
//     (FFrame::Object) that is an EndAIControllerEnemyBase (BossBase and every enemy AI blueprint, the summon fights'
//     included; party summons, party AI, UI and level blueprints are not), and passes everything else through.
// Who may be "the player" is the Condition select's rule (player_two/equal_targeting): each player's character must be
// a candidate for that enemy's target selector; a coin when both are. A coin naming player 2 is checked again at each
// answer, and tossed anew once player 2's character is no longer a candidate. Nothing is written to the party
// manager. Not covered (native reads of the leader): the opening target, lock groups (Stun Ray), the LEADER_ point.
//
// Any thread: enemy AI may run on workers. The coins live in a lock-free table keyed by the controller.
// Logged: the install and the UFunction swaps (Info), each toss (Debug), a summary every 60 s (Debug), each answer
// (Trace).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::target_coin {

/// Replaces the decision slots (logged). The UFunction swaps follow in Tick, once the reflection can be read. Needs
/// equal_targeting and player_hits.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per frame: the UFunction swaps until they are in place, and the summary.
void Tick() noexcept;

}  // namespace lss::player_two::target_coin
