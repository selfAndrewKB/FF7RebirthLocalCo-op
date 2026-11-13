#pragma once

// Climbing walls, monkey bars and spider walls read their input as each climber's own (W5,
// research/RE-p2-traversal-climb-context.md).
//
// These moves tick on the anim worker, outside every scope of the mod. There the game asks "is this the player?" (the
// leader test, no for a seat's member), takes its stick from the merged input (whichever pad pushes harder) and its
// basis from the primary camera manager (player 1's). A seat's member then runs the NPC route: she climbs by herself
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
// edge, so a seat's lean ended and restarted every few frames (a stutter) unless player 1's camera faced the ledge. Its
// Tick runs in the player's context with the identity suspended: only the key values and the camera manager getter
// answer (a seat: its pad and its camera; player 1: pad 1). It is not a climb (PlayerHere and TicksPlaced leave it
// out). Logged: Debug per action change, Trace per Tick, and Info when a lean restarts 3 times within 2 s.
//
// The field dodge (research/RE-dodge-steer.md) turns the character toward the stick on every update of its three
// phases, gated by the leader test (outside a context: no turn for a seat's member, and the merged stick of every pad).
// Its Tick runs in the player's full context, as a climb's (a seat: identity, pad, camera; player 1: pad 1), but it is
// not a climb. Its Tick tree records nothing into another player's controller; its Finish does, and is suspended as
// the climbs' is. Logged: Debug at each dodge's start and end with the character's facing, so a turned dodge shows.
//
// The landing after a high drop (research/RE-landing-roll.md) is the edge-stop action's phase 6: at its enter (the game
// thread, inside the action's Begin) the game plays the roll when the action's input direction is longer than half,
// and the roll turns toward that direction on each update (the worker, inside the action's Tick). That direction is
// the player's stick only for the player test; for a seat's member outside a context it is its AI's walk goal or the
// way to the party leader (always full length: a roll toward player 1's area, every time). The phase's handler runs in
// the landing player's full context on either thread, the Tick's suspended edge-stop context lifted for the phase
// alone (a seat: identity, pad, camera; player 1: pad 1, so no other pad decides player 1's landing). Logged: Debug at
// each landing's start and end with the character's facing.
//
// The copies are taken in the engine-tick pre-callback (Tick below), after the game's merge and before the world tick:
// the worker reads the same frame's values. Every answer outside a context, and on every other thread, is the game's.
// Per seat: the input copies, presses, camera manager, member type, climb frames and counts.

#include "players/seats.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::input::climb_context {

/// Replaces the three climbs', the edge-stop action's and the field dodge's Tick and Finish slots, the landing's phase
/// handler, the hold-select call and the three getters (each checked; a failed check leaves that part to the game,
/// logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per engine tick, before the session's tick: this frame's input copies, weak values and the
/// members' types (each seat's cleared while its player is out), and the Debug summary.
void Tick() noexcept;

/// The player type the leader test answers yes for on this thread now (inside a seat's climb context, not
/// suspended), or -1. Any thread; called by member_movement's leader test.
int LeaderTypeHere() noexcept;

/// Whose climb this thread is ticking now, by player number (1 player 1, 2 player 2, 3 player 3: a seat's number plus
/// one), or 0 outside every context. Any thread.
int PlayerHere() noexcept;

/// The seat whose move this thread is ticking now (PlayerHere's player as a seat), or 0: player 1's, or none. Any
/// thread.
int SeatHere() noexcept;

/// The climbs' Tick slots are placed (each climb's Tick names its player on its thread): what the seats' own climb
/// holders need.
bool TicksPlaced() noexcept;

/// The named move's Tick slot is placed (a move another module's levers depend on: the hookshot's, the hook rope's).
bool MovePlaced(const wchar_t* name) noexcept;

/// Runs `call(context)` on this thread with the primary camera manager getter answering `seat`'s, and no other answer
/// of the seat's (the context is suspended: the leader test and the buttons stay the game's). Any thread. False, and
/// not run, when the getter is not redirected or the seat's camera manager is not known this frame.
bool WithSeatCamera(int seat, void (*call)(void*), void* context) noexcept;


}  // namespace lss::input::climb_context
