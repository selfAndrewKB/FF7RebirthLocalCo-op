#pragma once

// The seats' trigger pass (research/RE-p2-trigger-overlap.md §5 Lever 2; plan/topics/traversal.md, the trigger pass).
//
// Trigger events come from each AEndPlayerTrigger's own tracker (+0x230): it queries the capsule's overlaps every tick
// and adds the key PlayerTrigger to the trigger registry's pending set. The field tick resolves that key to the PRIMARY
// controller's trigger only, reads that tracker's query result and hands it to TriggerTracker_DiffAndDispatchEnterLeave,
// which raises the enters and leaves. A seat's tracker queries every tick, but nothing reads its result.
//
// The game has no path for a second PlayerTrigger, so this is a documented recreation of the field tick's per-tracker
// body (about 20 lines): right after the game's dispatch of player 1's tracker, in the same frame and with the field
// tick's own scratch structures, each seat's tracker's result is read as the field tick reads it and passed to the
// game's own diff. Everything after the diff is the game's.
//
// The diff's enter and leave filter calls are gated for a seat's trigger (traversal L1, research/RE-p2-traversal-L1.md).
// The actor is found by the key in the game's tag registry, as the routers find it. Admitted classes (ladders, climbing
// walls, crawls, squeezes, beams, monkey bars, lifting ropes, spider moves, auto-start zip lines, pole slides, one-shot
// moves, wall runs: research/RE-p2-traversal-classes.md; chocobo climbs, also while the seat's player rides their own
// chocobo: research/RE-p2-chocobo-climb.md §3.1) go through the game's filter as that player: inside a LeaderScope
// of the seat's member and a primary-controller scope, so the spot registers on (and leaves) the seat's controller's
// list and the enter/leave checks read the seat's tracker, with the seat's own entered flag (+0x2A1), and for pole
// slides, one-shot moves and chocobo climbs the seat's own stored character, swapped in for the call. The wall-run move state's camera
// pick reads the seat's flags the same way. Every other event is logged and held back: the router would register it on
// player 1's list, switch map layers or fire story triggers for the seat's player.
//
// Per seat (players/seats.h): after player 1's tracker each active seat's tracker is
// read and diffed in seat order, one pass completing before the next; each seat has its own shadow of the entered
// flags (and stored characters), its counts and its wall-run camera pick; a delivery opens the seat's ladder (the
// primary controller and the leader answer that seat). Deliveries never nest. The ridden chocobo comes from
// chocobo_ride, by seat.

#include "platform/image.h"

namespace lss::world::seat_triggers {

/// Takes over the field tick's tracker dispatch call (checked bytes) and finds the game functions the pass uses. The
/// two filter calls go in from the game thread (Tick): in each build one of them crosses a 16-byte block.
void Install(const platform::GameImage& image) noexcept;

/// Puts the calls back. The filter calls only from the game thread outside the field tick (the mod's unload path).
void Remove() noexcept;

/// Each engine tick, game thread: places the filter calls once, and writes the 10 s summary.
void Tick() noexcept;

}  // namespace lss::world::seat_triggers
