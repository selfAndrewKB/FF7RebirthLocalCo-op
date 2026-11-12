#pragma once

// Logs when a seat's character is stranded off the loaded navmesh and asks the game's follow warp to bring
// it back (RE-p2-party-leash.md, RE-p2-party-warp.md; the ridden chocobo: RE-p2-chocobo-warp.md).
//
// Field characters may walk only on navmesh, and the navmesh is streamed in a 3x3 block of 120 m cells
// around the party leader. When the character stands where the block has unloaded, each move it asks for
// finds no navmesh, the game asks EndWorld_IsReadyAt_NavCellAndGrids about its spot, and on "not ready"
// it zeroes the move: the character runs in place. That call, inside EndCMC_ConstrainMoveDelta_NavMeshOnly, is
// redirected so the mod sees the game's own answer; the answer is passed back unchanged. Only moves the
// character asked for reach it (a zero move skips the navmesh step), so standing still is never a refusal.
// The falling step asks the same question (EndCMC_NewFallVelocity_WorldReadyFreeze, RE-p2-party-leash.md)
// and on "not ready" zeroes the fall, every tick, so a character that stepped off an edge hangs in the air;
// that call is redirected too and counts the same way, stick or no stick. A character frozen on its last safe
// spot is then parked by SE's safe-transform system: MOVE_None, so neither step runs, while its 0.12 s
// check keeps asking the question with the nav clause off. That third call is redirected as well; its "not
// ready" counts only while the game's park bit (stop reason 5) is set, since short of the park the game
// return-warps the character to its safe spot itself.
//
// Two characters are watched, each its own subject with the same step: a seat's member on foot, and the
// chocobo its player rides (its moves go through the same three calls). StrandWatch (world/strand_watch.h)
// turns a subject's refusals into began / warp / ended; at "warp" its own follow warp brings it back
// (world/party_warp.h; the chocobo's, with that player seated). The game moves characters on worker threads
// as well as the game thread, so the redirects only count (atomics) and Tick, on the game thread, turns the
// counts into events. Inert unless a seat is in. The lines carry the subject's player's and player 1's positions and
// the leader's nav cell; the AI probes and the once-a-second status line are dev tools.
//
// Per seat (players/seats.h): each seat has its two subjects (its member and its chocobo), their counts, strands and
// tick clock; the redirects find the subject by its root among every seat's, and Tick steps the seats in order. One
// warp at a time (party_warp.h): a seat whose strand reaches "warp" while another seat's warp runs waits (logged) and
// asks once that warp is over, if its strand still holds. The chocobos come from chocobo_ride, by seat.
//
// A player asks too: L3 clicked twice within 0.4 s on player 2's or 3's own pad (the engine key's own element in the
// game's action tables, read through that seat's view, input/pad_buttons TakeLeftThumbPress; the rule in
// world/party_warp_rules.h) asks the same follow warp for the chocobo that player rides, else their member,
// as a strand's "warp" does; behind another seat's warp it waits and is asked again once that warp is over. Not when
// the character already stands beside player 1's (within 10 m on the same level, wider than where the warp itself lands
// a follower: a stray double click moves nothing);
// every other refusal is party_warp's (the split not on screen, a ride, a battle, no follow state, the party busy),
// logged. Player 1's double click does nothing: theirs is the character the others are brought to. The asks need only
// the warp, not the three redirects. Logged at Info: each ask and its answer.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::world::nav_strand {

/// Redirects the three calls (the walking one required); logged either way. False if a check failed (nothing installed:
/// the strands are not watched; a player's ask still is heard).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, after the session's. `splitShown`: the split is on screen (a warp needs it).
void Tick(bool splitShown) noexcept;

#if LSS_DEV
/// The `party-warp <player>` command (asks as the double click does). May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::world::nav_strand
