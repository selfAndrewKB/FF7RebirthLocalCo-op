#pragma once

// Logs when player 2's character is stranded off the loaded navmesh and asks the game's follow warp to bring it back
// (RE-p2-party-leash.md, RE-p2-party-warp.md; the ridden chocobo: RE-p2-chocobo-warp.md §1, §4).
//
// Field characters may walk only on navmesh, and the navmesh is streamed in a 3x3 block of 120 m cells
// around the party leader. When the character stands where the block has unloaded, each move it asks for
// finds no navmesh, the game asks EndWorld_IsReadyAt_NavCellAndGrids about its spot, and on "not ready"
// it zeroes the move: the character runs in place. That call, inside EndCMC_ConstrainMoveDelta_NavMeshOnly, is
// redirected so the mod sees the game's own answer; the answer is passed back unchanged. Only moves the
// character asked for reach it (a zero move skips the navmesh step), so standing still is never a refusal.
// The falling step asks the same question (EndCMC_NewFallVelocity_WorldReadyFreeze, RE-p2-party-leash.md §6c)
// and on "not ready" zeroes the fall, every tick, so a character that stepped off an edge hangs in the air;
// that call is redirected too and counts the same way, stick or no stick. A character frozen on its last safe
// spot is then parked by SE's safe-transform system (§6d): MOVE_None, so neither step runs, while its 0.12 s
// check keeps asking the question with the nav clause off. That third call is redirected as well; its "not
// ready" counts only while the game's park bit (stop reason 5) is set, since short of the park the game
// return-warps the character to its safe spot itself.
//
// Two characters are watched, each its own subject with the same step: player 2's member on foot, and the
// chocobo player 2 rides (its moves go through the same three calls). StrandWatch (player_two/world/strand_watch.h)
// turns a subject's refusals into began / warp / ended; at "warp" its own follow warp brings it back
// (player_two/world/party_warp.h; the chocobo's, with player 2 seated). The game moves characters on worker threads
// as well as the game thread, so the redirects only count (atomics) and Tick, on the game thread, turns the
// counts into events. Inert unless player 2 is in. The lines carry both players' positions and the leader's
// nav cell; the AI probes and the once-a-second status line are dev tools.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::nav_strand {

/// Redirects the three calls (the walking one required); logged either way. False if a check failed (nothing installed).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, after the session's. `splitShown`: the split is on screen (a warp needs it).
void Tick(bool splitShown) noexcept;

}  // namespace lss::player_two::nav_strand
