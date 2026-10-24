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
// move's character weak at +0x450) by player number: 1 player 1, a seat's number plus one (2 player 2, 3 player 3;
// players/seats.h). The getters and the pressed test name no player: they read this context (the "inside seat"
// context). Inside it, on that thread only:
//   - a seat: the leader test answers yes for components of the seat's member's player type, the primary camera manager
//     getter returns the seat's, and the key-value getter reads a copy of the input owner's key fields taken this frame
//     inside StickScope(the seat's pad), so the game's own stick, dead zone and basis code runs on the seat's input. The
//     hold-select's Catch / FieldAction / Dodge read the seat's presses, recorded this frame inside a ButtonScope with
//     the game's own pressed test.
//   - player 1: only the key-value getter, with pad 1's copy: no seat's pad steers player 1's climb.
// Finish (which runs inside the Tick) drops the seat's identity: its leader-action record would go to player 1's
// controller. The hold-select drops it while another player also climbs: its prompt point is one global. Zip lines are
// left out (their rumble plays on pad 1).
//
// The edge-stop action (ledge lean, the step after an edge stop, the walk step; research/RE-p2-ledge-peek.md) ticks the
// same way. Its lean holds only while the stick, turned by the primary camera's yaw, points within 60 degrees of the
// edge, so player 2's lean ended and restarted every few frames (a stutter) unless player 1's camera faced the ledge. Its
// Tick runs in the player's context with the identity suspended: only the key values and the camera manager getter
// answer (a seat: its pad and its camera; player 1: pad 1). It is not a climb (PlayerHere and TicksPlaced leave it
// out). Logged: Debug per action change, Trace per Tick, and Info when a lean restarts 3 times within 2 s.
//
// The copies are taken in the engine-tick pre-callback (Tick below), after the game's merge and before the world tick:
// the worker reads the same frame's values. Every answer outside a context, and on every other thread, is the game's.
// Per seat: the input copies, presses, camera manager, member type, climb frames and counts.

#include "players/seats.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::climb_context {

/// Replaces the three climbs' and the edge-stop action's Tick and Finish slots, the hold-select call and the three
/// getters (each checked; a failed check leaves that part to the game, logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick, before the session's tick: this frame's input copies, weak values and the
/// members' types (each seat's cleared while its player is out), and the Debug summary.
void Tick() noexcept;

/// The player type the leader test answers yes for on this thread now (inside a seat's climb context, not
/// suspended), or -1. Any thread; called by member_movement's leader test.
int LeaderTypeHere() noexcept;

/// Whose climb this thread is ticking now, by player number (1 player 1, 2 player 2, 3 player 3: a seat's number plus
/// one), or 0 outside every context. Any thread. (traversal_holders reads `== 2` as player 2's.)
int PlayerHere() noexcept;
static_assert(players::kSeatsBuilt == 1, "traversal_holders reads PlayerHere() == 2 as player 2's: before seat 2 is "
                                         "built it reads the player number as a seat (PlayerHere() - 1)");

/// All three moves' Tick slots are placed (each climb's Tick names its player on its thread).
bool TicksPlaced() noexcept;

/// Runs `call(context)` on this thread with the primary camera manager getter answering `seat`'s, and no other answer
/// of the seat's (the context is suspended: the leader test and the buttons stay the game's). Any thread. False, and
/// not run, when the getter is not redirected or the seat's camera manager is not known this frame.
bool WithSeatCamera(int seat, void (*call)(void*), void* context) noexcept;

/// Player 2's (traversal_exec, the rides group's, not yet per seat).
inline bool WithPlayerTwoCamera(void (*call)(void*), void* context) noexcept
{
    return WithSeatCamera(players::kPlayerTwoSeat, call, context);
}

}  // namespace lss::player_two::climb_context
