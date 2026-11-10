#pragma once

// No player switches onto a party member another player plays; a player's own switch takes any member no human holds
// (RE-p2-party-switch.md, pass S-SW1; RE-p2-own-battle-switch.md, pass S-OWNSW; every seat's member since
// plan/seats.md step 6; the turned rule inside a seat's tick as Remake's battle/party_switch).
//
// The game's battle switch (BattleLeader_CycleSwitch) walks the deployed members from the leader,
// one step either way with wrap-around, and takes the first that has its battle component and is
// neither dead nor petrified; CanSwitch counts the members that pass the same test. Both ask one
// component getter per member, and a null component is the game's own "skip this member" branch.
// Their two calls to it are redirected: outside a seat's command pass a seat's member answers null, so player 1's
// d-pad switch, the KO and petrify auto-switches, the vehicle's switch and the deferred hand-over pass over it;
// inside a seat's pass (its own controller's switch step, run with that player as the leader,
// battle/command_menu_tick.h) player 1's character and every other seat's member answer null instead, so the player
// takes any member no human holds, the field leader included, and never another human's. The session then plays the
// member the game moved the seat's controller onto (session/session.h, the own switch).
//
// The switch widget greys a neighbour portrait with its own test (dead or petrified). Its two calls
// are redirected too: the members the pass may not take read as not switchable, and the game plays its own
// "cannot switch" animation.
//
// A downed player switches too. The game's forced switch for a downed leader is refused in co-op (the player stays on
// their character until revived), and a manual switch from a downed leader meets the game's two-standing gate
// (CanSwitch), which a two-player party never passes. So the request's jump into the cycle is redirected: in co-op,
// for a leader that cannot act, the cycle runs without that gate, as the game's own forced switch runs it; the press
// is the player's. In a seat's pass the request never asks for the targeted hand-over (Guard held): its executor runs
// outside every pass and would move player 1 (a written exception; the native route is in the research, §2.3).
// Refusals and requests are logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::session::party_switch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Another player is in with a character (one they drive or ride along with): co-op, where the game's forced switch
/// leaves a downed player 1 on their character. Game thread.
bool AnotherPlayerPlays() noexcept;

}  // namespace lss::session::party_switch
