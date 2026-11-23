#pragma once

// Each player's own chocobo, with the experimental option own-chocobo on (off by default: players 2 and 3 ride along
// with player 1's party ride): player 2 or 3 calls a chocobo with their own press, mounts it and gets off by their own
// press; player 1's ride never mounts or dismounts them, and the AI members ride with player 1 as the game has them
// (research/RE-chocobo-summon-seats.md, RE-chocobo-gaps-seats.md). Whatever the option, a chocobo a player 2 or 3 is
// about to drive is not sent to follow player 1 as their member mounts.
//
// The game's whistle is party-level: the summon row's press plays the whistle on the character, its anim notify sends a
// call that names no character, and the ride manager brings one chocobo to the field leader. The members ride only on
// the party's ride: the ride manager's member entries spawn a chocobo at each deployed member's side and seat them while
// it answers "the party rides". The mod uses those parts as they are:
// - the press: the summon body, which the member tick skipped for players 2 and 3, runs for their member in the tick's
//   scopes (their primary controller, pad and leader answer), so the row, the gates and the press are theirs. Inside it
//   the party-level outputs are held back (the row never reads "stay", an order for player 1's chocobo; the walk-in
//   ride, the scooter and buggy rides and the buggy's row answer no), the ride manager takes their call whatever player
//   1's chocobo does, and the row's check that the character has the whistle animation (Cloud and Tifa alone have it)
//   answers "found" for every member: a member without it gets the whistle's sound and their call at the press;
//   Cloud and Tifa whistle with their animation, whose notify makes the call. The body's last part (a statue's press)
//   runs for their member as it is;
// - the call: the whistle's notify, when the whistling character is player 2's or 3's member, skips the party's call
//   and marks that player's own call instead;
// - the ride: in the member entries' "the party rides" answer, for an entry whose member player 2 or 3 plays, their own
//   call stands for the party's ride flag and player 1's ride never does (a fight, an abort, a getoff in progress still
//   answer no), so the game spawns that member's chocobo at their side and seats them (the ride module then boards it for
//   the player), and player 1's mount and getoff leave them as they are; as their member mounts, their chocobo is not
//   sent to follow player 1 (it waits by them);
// - the getoff: their Vehicle press, the game's party getoff, ends their own call instead (it would dismount
//   everyone), and the entry's answer goes back to no, which dismounts their member alone;
// - the game's forced getoffs (a fast travel, the field step, an event, an interaction, a level command; seen by
//   rides/wheelie_ride's redirects, which reach player 1's ride) end every player 2's and 3's own call too, so the ride
//   manager dismounts their member as it dismounts the whole party in single player: while the party ride is on, player
//   1's coordinator answers "riding" until every member's entry is idle, and a fast travel's field step waits for that;
// - a ride the game puts them on on purpose becomes their own call once it seats them: a story party mount (their entry
//   takes the game's answer while the story waits for every member's chocobo; a ride of their own already under them ends
//   first, or the story would wait for it); a party mount of the game's that seats everyone, told at the rise of the
//   ride manager's party flag by its instant byte (a fast travel with the chocobos, a ride restored with a save, the
//   chocobo time attack) or by the story's script ride of the leader (a call redirect notes its request, which otherwise
//   reads as player 1's walk-in); and a ride already under them as they join or take a member. Player 1's own whistle,
//   walk-in and re-ride leave them as they are;
// Whatever the option: a rider without the rider's stop animations (all but Cloud and Tifa) pulls the reins at a chocobo
// stop instead of showing no pose (the rider's action asks the rider's interface for the chocobo's animation by name;
// the slot answers a riding clip of theirs, once per new name), and players 2 and 3's idle coordinators show no walk-in
// mount icon over player 1's chocobo (that question, in their input pass, is about the party leader).
// Player 1's whistle and ride, and the AI members' rides, are the game's. Logged: Info at install, per call, per getoff,
// per ride kept and per call given up; Debug per entry state change of a calling player's member, per whistle, per follow
// held back, per story mount, per rider pose answered and once per player for the icon.
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

/// After one of the game's forced getoffs (`what` names its caller), outside every player's scope, whatever player 1
/// rode: each player 2's or 3's own call ends, so the game dismounts their member. An `instant` getoff removes the
/// chocobos at once, so each player 2 or 3 who drives a chocobo, on their own call or the party's ride, has their ride
/// ended first by the game's end of their coordinator (rides/chocobo_ride Release), while the chocobo is still there.
/// Logged per call and ride ended. Game thread.
void EndForForcedGetOff(const wchar_t* what, bool instant) noexcept;

}  // namespace lss::rides::chocobo_call
