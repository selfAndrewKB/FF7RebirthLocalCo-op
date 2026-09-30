#pragma once

// Logs when player 2's member is stranded off the loaded navmesh (RE-p2-party-leash.md, RE-p2-party-warp.md).
//
// Field characters may walk only on navmesh, and the navmesh is streamed in a 3x3 block of 120 m cells
// around the party leader. When the member stands where the block has unloaded, each move she asks for
// finds no navmesh, the game asks EndWorld_IsReadyAt_NavCellAndGrids about her spot, and on "not ready"
// it zeroes the move: she runs in place. That call, inside EndCMC_ConstrainMoveDelta_NavMeshOnly, is
// redirected so the mod sees the game's own answer for player 2's member; the answer is passed back
// unchanged. Only moves the member asked for reach it (a zero move skips the navmesh step), so standing
// still is never a refusal. The falling step asks the same question (EndCMC_NewFallVelocity_WorldReadyFreeze,
// RE-p2-party-leash.md §6c) and on "not ready" zeroes her fall, every tick, so a member who stepped off an
// edge hangs in the air; that call is redirected too and counts the same way, stick or no stick. A member
// frozen on her last safe spot is then parked by SE's safe-transform system (§6d): MOVE_None, so neither
// step runs, while its 0.12 s check keeps asking the question with the nav clause off. That third call is
// redirected as well; its "not ready" counts only while the game's park bit (stop reason 5) is set, since
// short of the park the game return-warps her to her safe spot itself. StrandWatch (player_two/strand_watch.h)
// turns the refusals into began / warp / ended; at "warp" the game's follow warp brings her back
// (player_two/party_warp.h).
//
// Each line also carries what the warp design needs from a live run: both players' positions, the
// leader's nav cell, the member's movement over the strand (the running-in-place check), and her
// retained AI's current state (whether it is still the follow state the game's warp lives in).
// The game moves characters on worker threads as well as the game thread, so the redirect only counts
// (atomics) and Tick, on the game thread, turns the counts into events. Inert unless player 2 is in.

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
