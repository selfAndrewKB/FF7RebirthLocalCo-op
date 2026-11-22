#pragma once

// Each player's own chocobo call: player 2 or 3 whistles with their own press, and only their member gets a chocobo and
// mounts it; player 1 and the others are untouched (research/RE-chocobo-summon-seats.md).
//
// The game's whistle is party-level: the summon row's press plays the whistle on the character, its anim notify sends a
// call that names no character, and the ride manager brings one chocobo to the field leader. The members ride only on
// the party's ride: the ride manager's member entries spawn a chocobo at each deployed member's side and seats them while
// it answers "the party rides". The mod uses those parts as they are:
// - the press: the summon body, which the member tick skipped for players 2 and 3, runs for their member in the tick's
//   scopes (their primary controller, pad and leader answer), so the row, the gates and the whistle are theirs; inside
//   it the party-level outputs are held back (the row never reads "stay", an order for player 1's chocobo, so the press
//   is the whistle; the walk-in ride, the scooter and buggy rides and the buggy's row answer no). The game's own gate
//   still asks whether the ride manager can take a call, so no player whistles while player 1's chocobo is out; the
//   body's last part (a statue's press) runs for their member as it is;
// - the call: the whistle's notify, when the whistling character is player 2's or 3's member, skips the party's call
//   and marks that player's own call instead;
// - the ride: in the member entries' "the party rides" answer, for an entry whose member is a calling player's, their
//   call stands for the party's ride flag (a fight, an abort, a getoff in progress still answer no), so the game spawns
//   that member's chocobo at their side and seats them (the ride module then boards it for the player);
// - the getoff: their Vehicle press, the game's party getoff, ends their own call instead (it would dismount
//   everyone), and the entry's answer goes back to the game's, which dismounts their member alone.
// Player 1's whistle and ride, and the party's ride for every member, are the game's. Logged: Info at install, per call,
// per getoff and per call given up; Debug per entry state change of a calling player's member and per whistle.

namespace lss::platform {
class GameImage;
}

namespace lss::rides::chocobo_call {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Inside a player 2's or 3's member tick (their scopes open): the game's summon body for their state, under the own-call
/// rules above. False, nothing called, when the module is not placed or no player's scope is open.
bool RunSeatSummon(void (*body)(void* state, float deltaTime), void* state, float deltaTime) noexcept;

/// `seat` rides, or is being brought, a chocobo of their own call: their Vehicle press is theirs (a getoff of their own).
bool OwnCall(int seat) noexcept;

/// Each engine tick, game thread: the calling players' entries followed (logged), a call given up when no chocobo came.
void Tick() noexcept;

}  // namespace lss::rides::chocobo_call
