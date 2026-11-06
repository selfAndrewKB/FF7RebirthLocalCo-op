#pragma once

// Player 1 never switches onto a party member a seat's player plays (RE-p2-party-switch.md, pass S-SW1; every seat's
// member since plan/seats.md step 6).
//
// The game's battle switch (BattleLeader_CycleSwitch) walks the deployed members from the leader,
// one step either way with wrap-around, and takes the first that has its battle component and is
// neither dead nor petrified; CanSwitch counts the members that pass the same test. Both ask one
// component getter per member, and a null component is the game's own "skip this member" branch.
// Their two calls to it are redirected: for a seat's member they answer null, so the game's own
// cycle passes over that member. That covers the d-pad switch, the KO and petrify auto-switches,
// the vehicle's switch and the deferred hand-over. With the seats' members the last ones standing,
// nothing passes: player 1 stays on the downed leader (the game's own state for a downed leader
// with Reraise; game over needs every member down).
//
// The switch widget greys a neighbour portrait with its own test (dead or petrified). Its two calls
// are redirected too: a seat's member reads as not switchable, and the game plays its own
// "cannot switch" animation. Refusals are logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::session::party_switch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::session::party_switch
