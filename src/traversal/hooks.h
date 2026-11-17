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
//  - the notify itself: each character's notify pass runs in that character's own tick, outside every player's scope.
//    The gun point's notify slot runs a seat's character's call inside the seat's primary scope and leader answer, so
//    what the game resolves there (the compass pin's component, the switch's player below) is the seat's;
//  - the point: one per actor, switched on and off by every holder's notify with its own answer, so two players'
//    notifies would flip it every frame. The game's switch (the gun's SetFieldActionMgrPoint, called from the notify
//    and the unregister; the rope's SetActive) is called with the OR of the players' wishes, each wish recorded from
//    the call's own argument and the player the notify ran for (else whose scopes are open: the unregister) (call
//    redirects on the gun's three call sites; an entry redirect on the rope's function; the actor's EndPlay, an
//    unconditional off, is left alone). The gun's switch also writes the point's "not usable from here" bit from the
//    call's answer (where the notified character stands, whether it can act): the switch is called with the first
//    wishing player's answer, player 1's while they wish, and each player's selection sees the point as their own
//    answer left it (GunPointViewFor; world/field_pickup sets the bits around player 1's update and each seat's
//    pass). The gun point's pin on the search compass goes to the primary controller's leader-action component, but
//    the game makes it only as the point goes from down to up and takes it down with the point: a player joining a
//    point another holds gets a down and an up of the setter in their own context, a player leaving one the game's
//    own removal of their pin;
//  - the press: the seat's field-pickup pass admits the gun's point by its presets and a delivered rope's point by its
//    key (AdmitsPoint), so the seat's press runs the game's own dispatcher and listener. The gun's press flag has no
//    owner in the game and every holder's notify of the press frame would start the hookshot on its own character
//    (research/RE-hook-follow.md): the flag is looked at after each seat's pass (a seat's press runs inside it), and
//    a flag that came up names its presser; one no pass named is player 1's, from the game's own input step, named
//    at the first notify that sees it or after the game's update, whichever comes first. The notify slot runs the game's with
//    the flag at 0 for a call whose character is another player's, so only the presser's character goes. The game
//    clears the flag on the point's off path, which the OR of the wishes holds back while another player still
//    holds the point, so the presser's leave (their wish off while the point stays on) makes that clear in its stead;
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
//  - the rope's landing prompt: one field-action point for the swing's landing target (the B to jump row), made by the
//    selection that picks it: the swinging seat's selection takes it and every other player's skips it
//    (LandingPointViewFor).
// Not handled: the rope's held Dash and Accel on the worker read the merged pads (not logged); the landing point's
// compass pin and world marker stay player 1's. Game thread, except the camera handle swaps and the End's finish path.
//
// Per seat (players/seats.h): the seat comes from the move's character, the primary controller of the open scope, or
// the climb context's player; the wishes are per actor and player; the handle copies per seat.

#include "world/field_rules.h"

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

/// Whether a field-action point is a gun point (by `preset`), a noted rope's (by `key`), or the rope's landing point
/// while a seat owns it: the seat's pass leaves it open (each player's view of the landing point: LandingPointViewFor).
/// Game thread (the landing point's sighting is noted for AfterLandingScan).
bool AdmitsPoint(std::uint64_t key, std::uint64_t preset) noexcept;

/// After a selection update ran for `player` (0: the game's own; a seat: its pass): a gun point whose press flag came
/// up since the last look was pressed in that update, by that player. Game thread.
void NotePressesAfter(int player) noexcept;

/// The view (world/field_rules.h) of the gun point keyed `key` for `player` (0 player 1, a seat). Game thread; reads
/// the game's memory only and notes the view in the mod's own table, with no call, allocation or log (it may run under
/// the field-action manager's point lock); a change is said by LogGunPointViews. One press flag serves the point: a
/// second player's press in the frame of another's is the game's one press, named to the first.
world::field_rules::PointView GunPointViewFor(std::uint64_t key, int player) noexcept;

/// Each player's view of each gun point that changed since it was last said (Debug). Game thread, outside the lock.
void LogGunPointViews() noexcept;

/// The hook rope's landing point (research/RE-rope-landing-prompt.md): the one field-action point a swing adds for the
/// target its search found, whose row is the B to jump prompt. Its key is the game's FName global, checked at install.
bool IsLandingPoint(std::uint64_t key) noexcept;

/// The view of the landing point (its `kind`, the point's kind byte) for `player` (0 player 1, a seat) while a seat
/// owns it (world/field_rules.h LandingView): the owner is the last seat whose swing began, handed to another seat
/// still swinging at its end, and kept until the game has removed the point. NotTracked when it is not the landing point
/// or no seat owns it (player 1's swing is the game's own). AdmitsPoint admits the point while a seat owns it. Game
/// thread; like GunPointViewFor, no call, allocation or log (it may run under the point lock).
world::field_rules::PointView LandingPointViewFor(std::uint64_t key, std::uint8_t kind, int player) noexcept;

/// After a scan of the points (`playerOneScan`: player 1's own selection, which asks about every point each tick): a
/// point an ended swing still owned and the scan no longer met is released to the game; each player's view that
/// changed is said (Debug). Game thread, outside the lock.
void AfterLandingScan(bool playerOneScan) noexcept;

}  // namespace lss::traversal::hooks
