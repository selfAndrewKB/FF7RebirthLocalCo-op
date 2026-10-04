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
//     (a stalled walk: the mod's movement of player 2's member fighting the state's walk, R-C2);
//   - the chest spawn scan (§14; Tick, every 30 frames while player 2 is in): CharaPop's score-sorted entries, and for
//     each chest within 80 m of either player its state and the selection's own tests (rank against the count budget,
//     score against the memory cut, keep reasons), with its distance to each player and to the population's centre,
//     logged when its state or the test holding it changes. It tells which rule keeps a chest near player 2 unspawned;
//   - the chest admission tests (RE-p2-world-streaming.md §9.11): the CharaPop key-list rebuild (every 5 s) admits a chest
//     key within 300 m of the leader only if a ground trace down from its spawn point hits "Terrain" and the world is
//     ready there (collision grids). An EntryRedirect on the rebuild (CharaPopKeyList_Tick_BuildStateMachine) gives the
//     list; CallRedirects of its trace issue, trace result and world-ready test give, for each chest key, the spawn
//     point, the trace's length, its hit count, the world-ready answer and reason. Each chest's round outcome (dropped
//     by which test, or admitted) with its distance to each player and to the list's leader point: Info when it changes,
//     Debug every round. It tells which test holds the chest player 1 must come within ~88 m of. The trace issue and the
//     world-ready calls cross a 16-byte block: installed from the first game-thread tick.
// Every line is Info (a press, an open and a chest's spawn change are rare) but the per-frame position (Trace) and the
// admission rounds that change nothing (Debug).
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
