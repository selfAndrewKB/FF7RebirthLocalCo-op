#pragma once

// Enemies aim their moves at one of the players, one coin per enemy action (research/RE-p2-enemy-target-bias.md).
//
// Enemy blueprints name "the player" with a few native functions: GetPartyLeader (the summons' signature lock-ons,
// distances and angles to the leader, lists that drop the leader), IsUCPC / IsAIPC (is this character the player, an
// AI member) and GetPartyLeaderType. In co-op these all name player 1, so every such move goes to player 1. The user's
// rule: each enemy tosses a coin when it decides its next action and counts that player as "the player"
// until its next decision, so a whole attack follows one player and checks made every frame do not flicker. Inside a
// damage or reaction call the player it is about answers instead (a counter aims at the player who hit it;
// battle/player_hits records which).
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
// Who may be "the player" is the Condition select's rule (battle/equal_targeting): each player's character must be
// a candidate for that enemy's target selector; a coin when several are. A coin naming a seat is checked again at each
// answer, and tossed anew once the seat's character is no longer a candidate. While player 1's character is down in
// co-op, a coin naming player 1 is tossed anew when another player's character stands, and otherwise answers the
// standing member battle/enemy_leader publishes (the AI ally the game's switch would have taken). Nothing is written to
// the party manager. Not covered (native reads of the leader): the opening target, lock groups (Stun Ray), the LEADER_ point.
//
// Every seat playing a member takes part: the coin is a uniform pick among the eligible players
// (player 1 and the seats), and a coin holds the player's number (battle/coin_table.h).
//
// Any thread: enemy AI may run on workers. The coins live in a lock-free table keyed by the controller.
// Logged: the install and the UFunction swaps (Info), each toss (Debug), a summary every 60 s (Debug), each answer
// (Trace).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::target_coin {

/// Replaces the decision slots (logged). The UFunction swaps follow in Tick, once the reflection can be read. Needs
/// equal_targeting and player_hits.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per frame: the UFunction swaps until they are in place, and the summary.
void Tick() noexcept;

/// A lock-group pick is a decision whose target holds until it dies: tosses `controller`'s coin now,
/// by the same rule, and keeps it as the enemy's coin. The player's number (1 player 1, seat + 1 a seat's player:
/// 2 player 2; coin_table::SeatOfNumber gives the seat), 0 none (off, no seat's member, or not an enemy controller; the
/// caller must already have checked that `controller` is a live object). Any thread.
int TossForLockPick(const void* controller) noexcept;

/// `enemy`'s AI controller, only when the controller's own enemy pointer names `enemy` back (the game's path to it:
/// character +0x588; research/RE-p2-enemy-target-bias.md). Null on any mismatch. Guarded reads; any thread.
const void* ControllerOfEnemy(const void* enemy) noexcept;

/// `controller` itself when its enemy character's controller pointer names it back, else null. Any thread.
const void* CheckedController(const void* controller) noexcept;

/// `controller` (a checked live object, or null) is an enemy AI controller (EndAIControllerEnemyBase or a child: every
/// enemy's and boss's, not the party's). False until the class is found. Any thread.
bool IsEnemyController(const void* controller) noexcept;

/// The coin `controller` holds now, for a native read of "the player" made for that enemy (research/RE-p2-leader-native-reads.md):
/// tossed if it has none, tossed again if it names a seat's player who is no longer a candidate (the
/// blueprint answers' rule). The player's number as TossForLockPick gives it, 0 none (off, no seat's member, or not an
/// enemy controller; the caller must already have checked that `controller` is a live object). Any thread.
int CoinFor(const void* controller) noexcept;

/// `to` takes the coin `from` holds now (a combination's partner aims where its initiator does), until its own next
/// decision. Nothing if `from` holds none or `to` is not an enemy controller (both must be checked live objects).
/// Any thread.
void ShareCoin(const void* from, const void* to) noexcept;

/// After a lock-group pick whose coin named a seat's player locked onto someone else (that player could not take it,
/// so the game's own player-1 term decided): `controller`'s coin becomes player 1, so its blueprint answers agree with
/// the game's single-player view of that pick. Any thread; nothing if the coin no longer names a seat's player.
void LockPickFellBack(const void* controller) noexcept;

}  // namespace lss::battle::target_coin
