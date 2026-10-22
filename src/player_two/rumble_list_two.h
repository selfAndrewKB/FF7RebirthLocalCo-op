#pragma once

// Each player's traversal rumble plays on their own pad (research/RE-p2-zipline-rumble-hook-who.md §4).
//
// The game keeps one global rumble list (zip line, chocobo, hookshot, ropes, doors, valves, vehicles, swimming: 48 add
// sites). An add carries no owner. Each controller's leader-action component ticks the list once per frame and plays
// its changes on whichever controller is primary at that moment. Player 2's component ticks first each frame, inside
// player 2's primary scope, so every effect played on pad 2, player 1's chocobo included, and each effect ran half as
// long (two ticks per frame).
//
// Player 2 gets a list of their own, made by the game's allocator and constructor at join and taken down the game's
// way at leave (its channels stopped on player 2's controller, then its own destructor). The add function sends an add
// to player 2's list when it is player 2's: on the game thread while player 2 is the primary player (player 2's
// scopes), or on a worker inside player 2's zip-line rumble block. The add itself is the game's, under the game's lock.
// The list tick of player 2's component builds and plays player 2's list with the game's own two functions, with player
// 2 as the primary player; player 1's component ticks the global list as before (never inside player 2's primary
// scope). The zip line's rumble gate answers yes for player 2's member, as it does for player 1's character.
//
// The game's stop by name follows the add (RE-p2-rumble-owner-chocobo.md §3.2): on the game thread while player 2 is the
// primary player, the list's owner names player 2's list for the length of the game's stop, under the game's lock. The
// footsteps of the chocobo player 2 rides (the chocobo's own virtual, called outside every scope) run inside player 2's
// primary scope, as the game runs the chocobo player 1 rides, so the game's gate asks player 2's coordinator and the
// add goes to player 2's list.
//
// Sources that run outside player 2's scopes name player 2 as the owner of their adds and stops while they run for
// player 2's scooter or chocobo (a per-thread owner; RE-p2-rumble-owner-chocobo.md §9): the scooter move state's slot 1
// (hit and loop) and slot 8 (the loop's stop), the sky glide's rumble delegate, the jump box's OnEnded, and the drift
// and brake actions. The last three often run on the anim worker. There the add goes to player 2's list under the game's
// lock. The drift and brake gates ask whether the chocobo's rider is the player, and the leader test answers yes for
// player 2's member on that thread (leader_answer::ThreadScope). In 1.004 the jump box's add is inlined: a few
// bytes at its list load, under the game's lock, take player 2's list on the thread the invoker named. A stop of player
// 2's on a worker is left (no source found does one; logged).
//
// Shared rides (player 1 drives the buggy or the bronco, player 2 stands aside): while player 2's component has a
// force-feedback manager of its own (ffb_manager_two.h), every add and stop on the game's list is copied onto player
// 2's list, so both pads play the ride (the vehicles add and stop by name, on the game thread). The ride's end stops on
// player 2's list whatever copy is left. Without player 2's own manager there is no copy: the shared manager's tracks
// would let player 2's copy play first and skip player 1's.
//
// Not routed: the hook rope and the hookshot (player 2 cannot start them). PS5 haptics stay single-target.
//
// Units: rumble_list_two.cpp (the add and the stop, the list tick, the zip line, player 2's list, the footsteps,
// install, removal and tick), rumble_list_copy.cpp (the shared ride's copy), rumble_sources_two.cpp (the sources
// outside player 2's scopes, placed from a table) and rumble_list_shared.h (what they share).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::rumble_list_two {

/// Replaces the add's and the stop's entries, the component tick's entry and its list-tick call, and the zip line's two
/// calls (each checked; all or none; logged), then the chocobo's footstep slot and the sources outside player 2's scopes
/// (each optional; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick: makes player 2's list once player 2 is in (and drops it if player 2 went out
/// another way), the member's weak value for the zip line, and the Debug summary.
void Tick() noexcept;

/// Game thread, while player 2 can still be the primary player (before the generation is withdrawn): stops player 2's
/// channels on player 2's controller and destroys player 2's list.
void PlayerTwoOut() noexcept;

/// Game thread, once per engine tick: copy the game's list onto player 2's (a shared ride with player 2's own
/// force-feedback manager) or not. Turning it off stops on player 2's list what was copied and not yet stopped.
void CopyPlayerOne(bool on) noexcept;

}  // namespace lss::player_two::rumble_list_two
