#pragma once

// Players 2 and 3 work the game's switches (levers, wall switches, lift panels: AEndFieldActionSwitchAccessActor) with
// their own pad, on their own character, as player 1 does (research/RE-further-player-levers.md).
//
// A switch is a field-action actor: a player's trigger registers it on the party leader's controller list
// (world/seat_triggers delivers a seat's enter as that seat's player, for the switches world/lever_rules.h lists), and
// each frame the notify of every character holding it places the switch's one field-action point. The player's press
// starts the point's hold in the field-action manager's slots (each seat's own copy in its world/field_pickup pass);
// the notify starts the switch move (control state 8) on its character once the hold progress is above 0, the move's
// executer polls the progress each frame (cancel at 0, complete at 1), the completed hold's press sets the actor's
// press latch, and the actor's tick fires the switch's success trigger and story flag: the level does the rest. What
// the game shares between the players, and what this does about each, the game's own lever first:
//  - the hold progress: read from the live manager's slot block, player 1's outside a seat's pass. The notify (a slot
//    of the switch class) and the executer's tick (a slot of its vtable) run, for a seat's member, with the seat's copy
//    of that block and the seat's carried target bits on the point held in the live manager for the call
//    (world/field_pickup's SlotHold), and the notify inside the seat's primary scope and leader answer, so its start
//    test, the point's "not usable from here" answer and the space test read the seat;
//  - the point: one per actor, switched on and off by every holder's notify with its own answer, so two players'
//    notifies would flip it every frame. The game's switch (the point setter, an entry redirect) is called with the OR
//    of the players' wishes, each wish recorded from the call's own argument and the player the notify ran for (else
//    whose scopes are open: the unregister). The setter writes the point's "not usable" bit from the call's own context,
//    so after each player's own call the bit is noted, and each player's selection sees the point as their own call
//    left it (PointViewFor; world/field_pickup sets the bits around player 1's update and each seat's pass). The
//    actor's own off calls (its tick's point-off request, its EndPlay) go to the game as they are;
//  - the in-range byte: one per actor, set by the register, cleared by the unregister; written as the OR of the players
//    in range after each (the class's register and unregister slots);
//  - the press latch: one per actor, and every holder's notify that sees it up would start the switch move on its own
//    character. The latch is looked at after each selection update (a seat's press runs inside its pass), and a latch
//    that came up names its presser; the notify runs with the latch down for every character but the presser's. The
//    game clears it on the point's off edge, which the OR of the wishes holds back while another player still holds
//    the point, so the presser's leave makes that clear in its stead;
//  - the press: the seat's field-pickup pass admits a delivered switch's point by its key (AdmitsPoint), so the seat's
//    press runs the game's own dispatcher and the switch's own listener.
// The start, the hold, the animations, the success trigger, the story flag and what the level does after are the
// game's. Game thread. Logged: Info at install, Debug per delivered switch, per presser, per executer phase seen, per
// view change.
//
// Per seat (players/seats.h): the seat comes from the notified character, the executer's character or the open scope;
// the wishes, views and in-range marks are per actor and player.

#include "world/field_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::levers {

/// Replaces the class's slots and the executer's tick slot and redirects the point setter; all or none (logged).
void Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, game thread: a seat that left the game wishes nothing and is in range of nothing (its notify and
/// leave never come); a switch nobody wishes on any more gets the game's off.
void Tick() noexcept;

/// The levers are placed (world/seat_triggers delivers a seat's switch enter only then).
bool Ready() noexcept;

/// The switch `actor` is one players 2 and 3 may use (world/lever_rules.h, by its name or its point's key).
bool Admits(const void* actor) noexcept;

/// A delivered switch's actor (its first tag is its point's key): the seats' passes admit that point from here on.
void NoteSwitch(const void* actor) noexcept;

/// Whether a field-action point is a noted switch's (by `key`): the seat's pass leaves it open. Game thread.
bool AdmitsPoint(std::uint64_t key) noexcept;

/// After a selection update ran for `player` (0: the game's own; a seat: its pass): a switch whose press latch came up
/// since the last look was pressed in that update, by that player. Game thread.
void NotePressesAfter(int player) noexcept;

/// The view of the switch point keyed `key` for `player` (0 player 1, a seat). Game thread; reads the game's memory
/// only and notes the view in the mod's own table, with no call, allocation or log (it may run under the field-action
/// manager's point lock); a change is said by LogPointViews.
field_rules::PointView PointViewFor(std::uint64_t key, int player) noexcept;

/// Each player's view of each switch point that changed since it was last said (Debug). Game thread, outside the lock.
void LogPointViews() noexcept;

}  // namespace lss::world::levers
