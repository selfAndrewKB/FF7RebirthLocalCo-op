#pragma once

// Which XInput users have a pad, asked of the game's own xinput1_3 once a second on the game thread (the game's input
// system polls the same API every frame; a disconnected slot's query is the slow one, so never per frame). Without it
// a dead pad beside a working one cannot be told from a scene or a lock in the log.
//   pads      the connected set, and per pad the packet number with the time since it last changed (a connected
//             pad that reports nothing is still "connected"; its packet number stands still)
// A change of the connected set logs at Debug. Each poll's set goes to input/pad_table's RecheckAnswering, which
// has the engine read again a slot whose pad answers while the engine counts it empty, and its gone pads to the player
// windows (a spare pad's Start takes back a player whose pad is gone). Game thread only; nothing here reads or writes
// game memory.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::input::pad_presence {

/// The pads that are gone (bit p: pad p, input/pad_rules' numbers): an XInput user connected at an earlier poll and
/// not at the last one, a second old at most, or a PlayStation pad that answered an earlier read of the pad table's
/// and not its last. A pad that has never answered since the start (a keyboard player 1's slot) is never gone. One poll
/// decides: a pad that drops for under a second counts as gone for that second (a spare pad's Start then could take
/// its player; accepted, as such a Start is rare). Game thread.
std::uint32_t GonePads() noexcept;

/// Once per frame on the game thread; asks XInput once a second.
void Tick() noexcept;

#if LSS_DEV
/// The `pads` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::input::pad_presence
