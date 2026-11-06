#pragma once

// The game's world actors (chests and the other WorldActorMgr items) pop in around a seat's player too
// (RE-p2-world-streaming.md §9.12; plan, "Chest render distance").
//
// Square Enix's WorldActorMgr (*(root+0x190)) holds one item per world actor TreasurePointMgr and others registered, each
// with a pop radius R (+0x18; 90 m for this field's chests, from DataTableWorldActorInfo) and an octree half-extent E
// (+0x20; 100 m). Each frame its tick (WorldActorMgr_Tick_LeaderProximitySpawn) requests a type-2 CharaPop spawn for an
// armed item whose octree box overlaps the party leader's bounding box and whose location is within R of the leader's box
// centre, and removes a spawned one beyond R. It takes one actor, the leader: there is no centre or override to set, so a
// chest near a seat's player and far from player 1 never pops.
//
// While a seat plays (and the NPC centre's switch is on, Ctrl+F10), every 10 ticks on the game thread: each item whose
// location is within its own R of a seat's character gets R and E widened by the players' separation (in 10 m steps,
// plus 5 m for the bounding-box centre), so the game's own test from player 1 holds it in exactly when a seat is near it
// (|leader - item| <= separation + |seat - item|). E changes through the game's own octree remove and add (the
// octree's vtable, as WorldActorMgr_OnOriginShift_ReinsertItems does). The game's tick still makes every spawn and remove
// decision. An item the seats leave (5 m past R) gets its values back, and with them the game's own removal by
// distance from player 1. All values are put back when no seat plays or the switch turns it off; a value the game rewrote
// (a re-registration) is taken as the game's.
// Logged: start and stop (Info), each chest widened or put back (Info; other items Debug), each chest's state within
// 150 m of either player as the game's tick moves it (Debug), a summary every 60 s (Debug).
// Game thread.
//
// Per seat (players/seats.h): an item is near when it is near any playing seat's
// member, and the widening is the widest separation between player 1 and a playing seat, so one write per item serves
// every seat. Shared: the widened items' record (one radius and extent per item, as the game has).

#include "players/seats.h"

#include <array>

namespace lss::platform {
class GameImage;
}

namespace lss::world::world_actor_reach {

/// Finds the game root (checked). Logged.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with, by seat, whether the seat's player plays a member (index 0 unused).
void Tick(const std::array<bool, players::kMaxPlayers>& plays) noexcept;

/// Puts the values back (at mod uninstall; on the game thread only, else logged).
void Release() noexcept;

}  // namespace lss::world::world_actor_reach
