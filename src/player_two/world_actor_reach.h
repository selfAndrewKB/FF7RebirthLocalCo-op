#pragma once

// The game's world actors (chests and the other WorldActorMgr items) pop in around player 2 too (RE-p2-world-streaming.md
// §9.12; plan, "Chest render distance").
//
// Square Enix's WorldActorMgr (*(root+0x190)) holds one item per world actor TreasurePointMgr and others registered, each
// with a pop radius R (+0x18; 90 m for this field's chests, from DataTableWorldActorInfo) and an octree half-extent E
// (+0x20; 100 m). Each frame its tick (WorldActorMgr_Tick_LeaderProximitySpawn) requests a type-2 CharaPop spawn for an
// armed item whose octree box overlaps the party leader's bounding box and whose location is within R of the leader's box
// centre, and removes a spawned one beyond R. It takes one actor, the leader: there is no centre or override to set, so a
// chest near player 2 and far from player 1 never pops.
//
// While player 2 plays (and the NPC centre's switch is on, Ctrl+F10), every 10 ticks on the game thread: each item whose
// location is within its own R of player 2's character gets R and E widened by the players' separation (in 10 m steps,
// plus 5 m for the bounding-box centre), so the game's own test from player 1 holds it in exactly when player 2 is near it
// (|leader - item| <= separation + |player 2 - item|). E changes through the game's own octree remove and add (the
// octree's vtable, as WorldActorMgr_OnOriginShift_ReinsertItems does). The game's tick still makes every spawn and remove
// decision. An item player 2 leaves (5 m past R) gets its values back, and with them the game's own removal by distance
// from player 1. All values are put back when player 2 stops playing or the switch turns it off; a value the game rewrote
// (a re-registration) is taken as the game's.
// Logged: start and stop (Info), each chest widened or put back (Info; other items Debug), each chest's state within
// 150 m of either player as the game's tick moves it (Debug), a summary every 60 s (Debug).
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::world_actor_reach {

/// Finds the game root (checked). Logged.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether player 2 plays a member.
void Tick(bool playerTwoPlays) noexcept;

/// Puts the values back (at mod uninstall; on the game thread only, else logged).
void Release() noexcept;

}  // namespace lss::player_two::world_actor_reach
