#pragma once

// Traversal for player 2, logging only (research/RE-p2-traversal.md §8, RE-p2-chocobo-traversal.md §12).
//
// Ladders, climbing walls, zip lines, squeezes, beams and the chocobo's climb walls and launch pads are field-action
// actors. When a player's trigger actor (controller +0x610) overlaps one, the actor registers itself on the party
// leader's controller list (+0x618); each player character then walks only its own controller's list each tick and
// asks every listed actor whether to start. Player 2's member and chocobo walk player 2's controller's list, which
// nothing fills, so player 2 is never offered these spots. This watch logs what the levers need to know:
//   - every trigger enter and leave by player 1's or player 2's trigger: the actor, the entered flag, and which
//     controller's list gained or lost the actor (decides lever L1 against L1b);
//   - both controllers' lists, on change;
//   - each player character's notify pass (its controller, how many actors it walks), on change;
//   - the ladder's per-frame offer (Ladder vt+0x748) per character, on change;
//   - a ladder climb's start and end, and whether its input update ran in player 2's scope;
//   - the chocobo's glide and water-jet state enter and exit: the chocobo, the thread, player 2's scope and the
//     camera manager the primary controller resolves to (the exit hazard of RE-p2-chocobo-traversal.md §4.3).
// Every hook passes the game's call through unchanged. Events at Debug, per-frame lines at Trace.

#include "platform/image.h"

#include <cstdint>

namespace lss::player_two::traversal_watch {

/// Hooks the trigger enter/leave, the notify pass's call, the ladder and its executer slots, and the glide and jet
/// state slots (checked bytes; each outcome logged).
void Install(const platform::GameImage& image) noexcept;

/// Puts every hook back.
void Remove() noexcept;

/// Each engine tick: both controllers' lists, on change. Game thread.
void Tick() noexcept;

/// The field tick's dispatch of one trigger tracker, with its overlap count: counted for the 10 s summary and logged
/// on change. Called by player_two/trigger_two, which owns that call. Game thread.
void NoteDispatch(const void* tracker, std::int32_t overlaps) noexcept;

}  // namespace lss::player_two::traversal_watch
