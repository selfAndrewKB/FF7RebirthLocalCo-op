#pragma once

// Traversal for the seats' players, logging only (research/RE-p2-traversal.md, RE-p2-chocobo-traversal.md).
//
// Ladders, climbing walls, zip lines, squeezes, beams and the chocobo's climb walls and launch pads are field-action
// actors. When a player's trigger actor (controller +0x610) overlaps one, the actor registers itself on the party
// leader's controller list (+0x618); each player character then walks only its own controller's list each tick and
// asks every listed actor whether to start. A seat's member and chocobo walk the seat's controller's list, which the
// game does not fill, so without the mod's levers a seat's player is never offered these spots. This watch logs what
// the levers need to know:
//   - every trigger enter and leave by a player's trigger: the actor, the entered flag, and which controller's list
//     gained or lost the actor (decides lever L1 against L1b);
//   - every player's controller's list, on change;
//   - each player character's notify pass (its controller, how many actors it walks), on change;
//   - the ladder's per-frame offer (Ladder vt+0x748) per character, on change;
//   - a ladder climb's start and end, and whether its input update ran in a seat's scope.
// The chocobo's glide and water-jet states are rides/chocobo_exits'. Every hook passes the game's call through
// unchanged. Events at Debug, per-frame lines at Trace.

#include "platform/image.h"

#include <cstdint>

namespace lss::traversal::traversal_watch {

/// Hooks the trigger enter/leave, the notify pass's call, the ladder and its executer slots (checked bytes; each
/// outcome logged).
void Install(const platform::GameImage& image) noexcept;

/// Puts every hook back.
void Remove() noexcept;

/// Each engine tick: every player's controller's list, trigger and trigger tracker, on change. Game thread.
void Tick() noexcept;

/// The field tick's dispatch of one trigger tracker, with its overlap count: counted for the 10 s summary and logged
/// on change. Called by world/seat_triggers, which owns that call. Game thread.
void NoteDispatch(const void* tracker, std::int32_t overlaps) noexcept;

}  // namespace lss::traversal::traversal_watch
