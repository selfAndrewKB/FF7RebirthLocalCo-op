#pragma once

// A seat's player inside CharaPop switch volumes (research/RE-p2-shop-npcs-near-p2.md §2, §6, §10).
//
// Some town NPCs (Costa del Sol's and Under Junon's shopkeepers) are on the show list of an AEndCharaPopSwitchVolume:
// CharaPop's spawn score excludes them (-999999) unless one of their volumes is "inside". The volume's own trigger enter
// and leave overrides set and clear that byte, and the game calls them only from the trigger filter, for the tracker it
// reads: player 1's trigger. So a shopkeeper, and the head icon bound to her, appeared near a seat's player only once
// player 1 came by.
//
// seat_triggers hands a seat's events of this class here, and the volume's own override is called with that seat's
// trigger's tag (PlayerTrigger, the same as player 1's), as the filter calls it. Not the rest of the filter: its route
// records the party leader as the entrant and notifies area managers. The filter's own on/off conditions hold
// (enter skipped while DisableTriggerBox 18 or DisableTriggerBox_InOut 110 is on, leave while 110 is). Only volumes
// with a show list and no hide list: a hide list despawns its entries everywhere, near player 1 too.
//
// "Inside" is the union of both players:
//   - a volume with extra shapes counts entries per key (+0x270), and both triggers share the key, so the game's own
//     count decides;
//   - a brush-only volume counts nothing (the weapon shop's), so either player's leave would clear it. There the mod
//     decides, a documented recreation (plan topics/world.md): a seat's leave is called only while player 1 is not
//     inside, and after player 1's leave the enter is called again for the seat while the seat is inside. Player 1 is
//     inside if the byte was already set at the seat's enter (only player 1's trigger sets it otherwise), or after player
//     1's own enter since.
// A seat's volumes are left (counted volumes always, brush-only ones unless player 1 is inside) when its player leaves
// the session or its trigger changes. Game thread only.
//
// Per seat (players/seats.h): one record per volume holds the mask of the seats inside
// it, each seat's own count of enters and whether player 1 is inside; each seat has its own trigger and tag. A
// brush-only volume calls its enter for the first player in only (a later seat is noted inside without a call) and its
// leave for the last one out (player 1 or another seat inside keeps it). The volume's inside byte is one write shared
// by every player.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::seat_switch_volumes {

/// Finds the class, its overrides (checked at their slots) and the data; logged either way. A row of the module table
/// after seat_triggers, whose filter delivers the events; without that filter it never fires.
void Install(const platform::GameImage& image) noexcept;

/// `actor` is a CharaPop switch volume (and the module is available).
bool Is(const void* actor) noexcept;

/// `seat`'s trigger's enter or leave of switch volume `volume` (seat_triggers' filter, inside the diff).
void SeatEvent(int seat, bool enter, void* volume, const void* trigger) noexcept;

/// A seat is inside a volume: player 1's events must be seen (AfterPlayerOne).
bool Watching() noexcept;

/// After the game's filter ran player 1's event for `actor` (null or any class: only a volume a seat is inside counts).
void AfterPlayerOne(bool enter, void* actor) noexcept;

/// Each engine tick, game thread: leaves each seat's volumes once that seat is gone, drops destroyed volumes, and
/// writes the 10 s summary (Debug, with activity).
void Tick() noexcept;

}  // namespace lss::world::seat_switch_volumes
