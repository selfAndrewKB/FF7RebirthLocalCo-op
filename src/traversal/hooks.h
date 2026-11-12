#pragma once

// The grappling gun's points and the hook ropes for players 2 and 3 (research/RE-hooks-for-seats.md,
// RE-p2-zipline-rumble-hook-who.md).
//
// Both are field-action actors a player's trigger registers on the party leader's controller list (world/seat_triggers
// delivers a seat's enter as the seat's player, so the actor lands on the seat's list); each frame the notify of every
// character holding the actor places the actor's one field-action point, and a press on it starts the action on that
// character (the gun: the next notify of any holder consumes a per-actor press flag; the rope: the press listener
// starts the swing on the party leader, the seat's member inside the seat's field-pickup pass). What the game shares
// between the players, and what this does about each, the game's own lever first:
//  - the point: one per actor, switched on and off by every holder's notify with its own answer, so two players'
//    notifies would flip it every frame. The game's switch (the gun's SetFieldActionMgrPoint, called from the notify
//    and the unregister; the rope's SetActive) is called with the OR of the players' wishes, each wish recorded from
//    the call's own argument and the player whose scopes are open (call redirects on the gun's three call sites; an
//    entry redirect on the rope's function; the actor's EndPlay, an unconditional off, is left alone). The gun
//    point's pin on the search compass goes to the primary controller's leader-action component, the seat's inside
//    the seat's scopes, but the game makes it only as the point goes from down to up and takes it down with the
//    point: a player joining a point another holds gets a down and an up of the setter in their own context, a player
//    leaving one the game's own removal of their pin;
//  - the press: the seat's field-pickup pass admits the gun's point by its presets and a delivered rope's point by its
//    key (AdmitsPoint), so the seat's press runs the game's own dispatcher and listener. The gun's press flag has no
//    owner in the game and every holder's notify of the press frame would start the hookshot on its own character
//    (research/RE-hook-follow.md): the listener notes who pressed, and the notify slot runs the game's with the flag
//    held at 0 for a call whose character is another player's, so only the presser's character goes;
//  - the follower path: the game's AI followers take a gun point after their leader through the actor's follower-enter
//    slot, and a seat's member, still possessed by its AI controller underneath, counts as a follower there. The slot
//    runs the game's for an AI member and answers 0 (the game's own answer for a character with no controller holder)
//    for a player's member;
//  - the start: the state switch queues, so the move's Setup (the gun spawned) and Begin run later in the member's
//    controller tick, outside the seat's scopes: the move's Begin and End slots run a seat's inside the seat's primary
//    scope and leader answer (the rope's Begin with the party leader answering the member too: its rail offset reads
//    the leader); the End's abort path is on the game thread, its finish path on the animation worker, where the
//    climb context (input/climb_context) already names the player for the move's Tick;
//  - the camera handle: the gun's is per actor (+0x9E0), the rope's one global; a second player's install would
//    overwrite it and the first's leave switch the second's module off. Each seat's own copy of the 16 bytes is swapped
//    in around the actor's installer and leave slots (the gun's per actor, the rope's under a lock), the seat known
//    from the Begin or End wrapper's thread on the game thread and from the climb context on the worker;
//  - the rumble: the gun's hit effect is gated by the game's player test on the game thread (a montage notify): for a
//    gun on a seat's member the test answers the seat's type on that thread and the seat owns the add
//    (feedback/traversal_rumble); the flight's effects on the worker are the climb context's (the move's Tick names
//    the seat as the owner).
// Not handled: the rope's held Dash and Accel on the worker read the merged pads (not logged). Game thread, except the
// camera handle swaps and the End's finish path.
//
// Per seat (players/seats.h): the seat comes from the move's character, the primary controller of the open scope, or
// the climb context's player; the wishes are per actor and player; the handle copies per seat.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::hooks {

/// Replaces the slots and the call sites (the gun's set together or not at all, the rope's likewise; the hit rumble
/// on its own; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// The gun's levers are placed (world/seat_triggers delivers a seat's gun point enter only then); the rope's likewise.
bool GunReady() noexcept;
bool RopeReady() noexcept;

/// A delivered rope's actor (its first tag is its point's key): the seat's pass admits that point from here on.
void NoteRope(const void* actor) noexcept;

/// Whether a field-action point is a gun point (by `preset`) or a noted rope's (by `key`): the seat's pass leaves it
/// open. Game thread.
bool AdmitsPoint(std::uint64_t key, std::uint64_t preset) noexcept;

}  // namespace lss::traversal::hooks
