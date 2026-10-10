#pragma once

// Climbing walls, monkey bars and spider walls read their input as each climber's own (W5,
// research/RE-p2-traversal-climb-context.md).
//
// These moves tick on the anim worker, outside every scope of the mod. There the game asks "is this the player?" (the
// leader test, no for player 2's member), takes its stick from the merged input (whichever pad pushes harder) and its
// basis from the primary camera manager (player 1's). Player 2's member then runs the NPC route: she climbs by herself
// toward player 1.
//
// A per-thread context, set by each move's Tick slot for the length of one Tick, names the climber's player (by the
// move's character weak at +0x450). Inside it, on that thread only:
//   - player 2: the leader test answers yes for components of the member's player type, the primary camera manager
//     getter returns player 2's, and the key-value getter reads a copy of the input owner's key fields taken this frame
//     inside StickScope(pad 2), so the game's own stick, dead zone and basis code runs on player 2's input. The
//     hold-select's Catch / FieldAction / Dodge read player 2's presses, recorded this frame inside a ButtonScope with
//     the game's own pressed test.
//   - player 1: only the key-value getter, with pad 1's copy: pad 2 no longer steers player 1's climb.
// Finish (which runs inside the Tick) drops player 2's identity: its leader-action record would go to player 1's
// controller. The hold-select drops it while player 1 also climbs: its prompt point is one global. Zip lines are left
// out (their rumble plays on pad 1).
//
// The copies are taken in the engine-tick pre-callback (Tick below), after the game's merge and before the world tick:
// the worker reads the same frame's values. Every answer outside a context, and on every other thread, is the game's.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::climb_context {

/// Replaces the three moves' Tick and Finish slots, the hold-select call and the three getters (each checked; a failed
/// check leaves that part to the game, logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick, before the session's tick: this frame's input copies, weak values and the
/// member's type (all cleared while player 2 is out), and the Debug summary.
void Tick() noexcept;

/// The player type the leader test answers yes for on this thread now (inside player 2's climb context, not
/// suspended), or -1. Any thread; called by member_movement's leader test.
int LeaderTypeHere() noexcept;

/// Whose climb this thread is ticking now (1, 2, or 0 outside every context). Any thread.
int PlayerHere() noexcept;

/// All three moves' Tick slots are placed (each climb's Tick names its player on its thread).
bool TicksPlaced() noexcept;

/// Runs `call(context)` on this thread with the primary camera manager getter answering player 2's, and no other
/// player-2 answer (the context is suspended: the leader test and the buttons stay the game's). Any thread. False, and
/// not run, when the getter is not redirected or player 2's camera manager is not known this frame.
bool WithPlayerTwoCamera(void (*call)(void*), void* context) noexcept;

}  // namespace lss::player_two::climb_context
