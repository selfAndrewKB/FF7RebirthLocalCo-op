#pragma once

// Player 2's trigger pass (research/RE-p2-trigger-overlap.md §5 Lever 2; the plan's "Design for player 2's trigger").
//
// Trigger events come from each AEndPlayerTrigger's own tracker (+0x230): it queries the capsule's overlaps every tick
// and adds the key PlayerTrigger to the trigger registry's pending set. The field tick resolves that key to the PRIMARY
// controller's trigger only, reads that tracker's query result and hands it to TriggerTracker_DiffAndDispatchEnterLeave,
// which raises the enters and leaves. Player 2's tracker queries every tick, but nothing reads its result.
//
// The game has no path for a second PlayerTrigger, so this is a documented recreation of the field tick's per-tracker
// body (about 20 lines): right after the game's dispatch of player 1's tracker, in the same frame and with the field
// tick's own scratch structures, player 2's tracker's result is read as the field tick reads it and passed to the
// game's own diff. Everything after the diff is the game's.
//
// The diff's enter and leave filter calls are gated for player 2's trigger (traversal L1, research/RE-p2-traversal-L1.md).
// The actor is found by the key in the game's tag registry, as the routers find it. Admitted classes (ladders, climbing
// walls, crawls, squeezes, beams, monkey bars, lifting ropes, spider moves, auto-start zip lines, pole slides, one-shot
// moves, wall runs: research/RE-p2-traversal-classes.md) go through the game's filter as player 2: inside a LeaderScope
// of player 2's member and a primary-controller scope, so the spot registers on (and leaves) player 2's controller's
// list and the enter/leave checks read player 2's tracker, with player 2's own entered flag (+0x2A1), and for pole
// slides and one-shot moves player 2's own stored character, swapped in for the call. The wall-run move state's camera
// pick reads player 2's flags the same way. Every other event is logged and held back: the router would register it on
// player 1's list, switch map layers or fire story triggers for player 2.

#include "platform/image.h"

namespace lss::player_two::trigger_two {

/// Takes over the field tick's tracker dispatch call (checked bytes) and finds the game functions the pass uses. The
/// two filter calls go in from the game thread (Tick): in each build one of them crosses a 16-byte block.
void Install(const platform::GameImage& image) noexcept;

/// Puts the calls back. The filter calls only from the game thread outside the field tick (the mod's unload path).
void Remove() noexcept;

/// Each engine tick, game thread: places the filter calls once, and writes the 10 s summary.
void Tick() noexcept;

}  // namespace lss::player_two::trigger_two
