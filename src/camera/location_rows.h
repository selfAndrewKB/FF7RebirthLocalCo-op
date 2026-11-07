#pragma once

// Each player's camera takes location camera rows from its own character only (RE-p2-framing.md: unscoped, the
// indoor row reaches every camera).
//
// Location volumes (towns, shops) change the camera through one global location manager. It tests the party
// leader's position, player 1's character, and sends the volume's camera row to every listener; every camera
// operator registers one in its constructor, so a seat's Field and Battle operators took player 1's indoor
// row while its player stood outside. Gate: the listeners' shared vtable slot is replaced, and a call whose
// operator is one of a seat's camera manager's (players/seats.h) is not forwarded (a skipped game call;
// nothing recreated).
// A seat's own rows: the game has no per-player route for this layer (the manager follows one character and
// also drives BGM, names and flags), so each tick the mod finds the volume each seat's character is in with the
// game's own volume list, containment test and rules (strictly inside to enter, the volume's UntouchMargin to stay,
// the lowest priority wins) and calls that seat's operators' own set/clear functions when it changes: a documented
// recreation of the enter/leave bookkeeping only (plan, RE-p2-framing.md §2.3).
//
// The InDoor nav-modifier volumes are detected per player controller by UEndPlayerExistNavAreaComponent, whose
// camera push goes to "the" primary camera manager. Its tick slot is replaced: a seat's controller's component
// ticks inside that seat's scopes::primary_controller::Scope, so the game's own push lands on the seat's camera (the game's
// path, run for the right player). Ticks and rows are counted and logged.
// Per seat: the location its camera holds and the counts. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::camera::location_rows {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: the counts, every 10 s while they change.
void Poll() noexcept;

/// Each engine tick: each seat's camera follows the location volume its own character is in, seat by seat.
void Tick() noexcept;

}  // namespace lss::camera::location_rows
