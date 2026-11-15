#pragma once

// The mine-cart climbs for players 2 and 3 (research/RE-cart-climb-seats.md).
//
// A cart's climb spot (the game's cart-jump actor) is a field-action actor a player's trigger registers on the party
// leader's controller list (world/seat_triggers delivers a seat's enter as the seat's player, for carts in the climb
// mode). Each frame the notify of every character holding it puts the cart on one shared climb list when that character
// can act and is heading for it, and takes it off otherwise; each player's own terrain detector reads that list for its
// own character and starts the climb (the game's own path for every player character). The AI followers climb through
// the follower path and never read the list. What the game shares between the players, and what this does about it:
//  - the notify: run for a character's own tick, outside every player's scope, while its can-act test reads the party
//    leader and the primary controller's pawn and its approach test the primary pawn's stick. The notify slot runs a
//    seat's character's call inside the seat's primary scope and leader answer, the player named meanwhile;
//  - the shared list: one entry per cart, which every holder's notify adds or removes with its own answer, so two
//    players' notifies would flip it every frame and either player's leave would take it away from both. The list's
//    add and remove (entry redirects, filtered to the cart's own three calls by their return addresses) take each
//    player's wish: the add is the game's (it adds a cart once), the remove the game's only when no player still wishes
//    the cart on. A notify's call is the named player's; the unregister's (a trigger leave) the open scope's player's;
//  - the follower path: a seat's member, still possessed by its AI controller underneath, counts as a follower there.
//    The slot runs the game's for an AI member and answers 0 for a player's member.
// The climb itself is the game's: its detector, its jump states, on the player's own character. Game thread. Logged:
// Info at install, Debug per wish change and per refused follower call.
//
// Per seat (players/seats.h): the seat comes from the notified character or the open scope; the wishes are per cart and
// player.

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::cart_climb {

/// Replaces the class's notify and follower slots and redirects the climb list's add and remove; all or none (logged).
void Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, game thread: a seat that left the game wishes nothing (its notify and leave never come); a cart
/// nobody wishes on any more goes off the list.
void Tick() noexcept;

/// The levers are placed (world/seat_triggers delivers a seat's cart enter only then).
bool Ready() noexcept;

/// The cart `actor` is a climb (its climb mode set), not a marker whose press jumps the party leader.
bool Admits(const void* actor) noexcept;

}  // namespace lss::traversal::cart_climb
