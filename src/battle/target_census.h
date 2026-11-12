#pragma once

// How enemies choose whom to attack, and how often they choose each player (research/RE-p2-enemy-target-bias.md).
// A dev tool: dev/dev_tools.h installs it; a player's build has the stubs below.
//
// Every enemy's target selector (Square Enix End code) is followed through the game's own calls. The census changes
// nothing: its own hooks pass every call on, and the two decisions it once hosted have their owners now (the
// Condition select's slot is battle/equal_targeting's, the lock-group pick calls are battle/lock_group_coin's;
// each tells the census what it did through an observer):
//   - each new selector (Info): the enemy, its kind (Hate, Condition, None) and, for every kind, the enemy
//     controller's TargetSelectConditions (how many elements name the UCPC, the AIPC or all);
//   - each decision (Debug): the reason, the target, whose character it is (player 1's, a seat's member, an AI
//     party member, other), and the stage that chose it (the kind's select, a lock slot, the redirect pair, the
//     Cait Sith decoy, or the current target kept), with the thread;
//   - each Condition draw (Debug): the drawn element's character class and condition, and whom the select counted
//     as "the UCPC" (player 1's or a seat's character, and why: coin, only one eligible, neither, or the game's own);
//   - each lock-group pick (Debug): the group, bPrioritizeUCPC, and whose character it picked;
//   - the Hate table at each Hate select (Trace);
//   - a summary every 60 s with decisions (Info): the shares per player, per stage and per kind, and the Condition
//     selects' UCPC answers.
// Hooks of its own: one entry redirect (Decide), two vtable slots (the Hate and None selects), two call redirects
// (creation, the Condition draw). Decisions run on worker threads as well as the game thread (the census's own
// finding), so nothing here assumes the game thread.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::target_census {

#if LSS_DEV

/// Places the hooks and registers the two observers (logged). After equal_targeting and lock_group_coin.
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

#else

inline void Install(const platform::GameImage&) noexcept {}
inline void Remove() noexcept {}

#endif

}  // namespace lss::battle::target_census
