#pragma once

// Logging only: how enemies choose whom to attack, and how often they choose each player
// (RE-p2-enemy-target-bias.md §6, hook facts §8, S-TGTCENSUS).
//
// The census that comes before any "equal bias" change. Every enemy's target selector (Square Enix End code) is
// followed through the game's own calls, and nothing it decides is changed:
//   - each new selector (Info): the enemy, its kind (Hate, Condition, None) and, for every kind, the enemy
//     controller's TargetSelectConditions (how many elements name the UCPC, the AIPC or all);
//   - each decision (Debug): the reason, the target, whose character it is (player 1's, player 2's member, an AI
//     party member, other), and the stage that chose it (the kind's select, a lock slot, the redirect pair, the
//     Cait Sith decoy, or the current target kept), with the thread;
//   - each Condition draw (Debug): the drawn element's character class and condition, and the leader type the
//     select read as "the UCPC";
//   - each lock-group pick (Debug): the group, bPrioritizeUCPC, and whose character it picked;
//   - the Hate table at each Hate select (Trace);
//   - a summary every 60 s with decisions (Info): the shares per player, per stage and per kind.
// Hooks: one entry redirect (Decide), three vtable slots (each kind's select), four call redirects (creation, the
// Condition draw, the two lock-group picks). Every hook passes the game's call on unchanged. The decision's thread
// is not known statically, so nothing here assumes the game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::target_census {

/// Places the hooks (logged). On by default: the census only reads.
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

}  // namespace lss::player_two::target_census
