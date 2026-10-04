#pragma once

// Logging only: chests, whoever uses them (RE-p2-field-interact.md §13.6; plan, "Player 2
// triggers field prompts").
//
// Since v0.53.0 player 2's field pass (player_two/field_pickup) admits chest points, so a chest player 2 presses runs
// the game's own listener and treasure state on player 2's member. Two questions are open (§13.4): whether the state's
// OnEnter runs inside the pass (then its leader test answers yes and the global menu locks apply for the open, as in
// single player; R-C1), and whether the mod's movement of player 2's member fights the state's walk to the chest (R-C2).
// Each hook passes straight to the game's function:
//   - an EntryRedirect on the chest listener TreasurePointMgr_OnFieldActionKey_RequestTreasureState (the press): the
//     key, whether player 2's pass ran it, the leader it will use (PM+0x148), its answer;
//   - a CallRedirect of OnEnter's leader test CharCtrlStateTreasure_IsCharLeaderOrRide: inside the pass or not, its
//     answer (R-C1);
//   - an EntryRedirect on the grant CharCtrlStateTreasure_GrantRewards_MarkOpened: logged only when the call set the
//     state's granted bit (+0x131 bit 0x20; the game also calls it as a no-op), with the frames since the press and how
//     far the opener moved;
//   - each frame of an open (Tick): the opener's position (Trace), and a WARNING when no grant came 6 s after the press
//     (a stalled walk: the mod's movement of player 2's member fighting the state's walk, R-C2).
// Chests' spawning around player 2 is player_two/world_actor_reach (RE-p2-world-streaming.md §9.12).
// Every line is Info (a press, an open and a grant are rare) but the per-frame position (Trace).
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::chest_watch {

/// Installs the hooks (each checked by its bytes). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Once per frame (the engine-tick callback): follows an open from its press to its grant.
void Tick() noexcept;

}  // namespace lss::player_two::chest_watch
