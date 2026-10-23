#pragma once

// Each player's camera takes location camera rows from its own character only (RE-p2-framing.md, "Live finding
// 2026-10-01: the indoor row reaches both cameras").
//
// Location volumes (towns, shops) change the camera through one global location manager. It tests the party
// leader's position, player 1's character, and sends the volume's camera row to every listener; every camera
// operator registers one in its constructor, so player 2's Field and Battle operators took player 1's indoor
// row while player 2 stood outside. Gate: the listeners' shared vtable slot is replaced, and a call whose
// operator is one of player 2's camera manager's is not forwarded (a skipped game call; nothing recreated).
// Player 2's own rows: the game has no per-player route for this layer (the manager follows one character and
// also drives BGM, names and flags), so each tick the mod finds the volume player 2's character is in with the game's
// own volume list, containment test and rules (strictly inside to enter, the volume's UntouchMargin to stay, the
// lowest priority wins) and calls player 2's operators' own set/clear functions when it changes: a documented
// recreation of the enter/leave bookkeeping only (plan, RE-p2-framing.md §2.3).
//
// The InDoor nav-modifier volumes are detected per player controller by UEndPlayerExistNavAreaComponent, whose
// camera push goes to "the" primary camera manager. Its tick slot is replaced: player 2's controller's component
// ticks inside primary_controller::Scope, so the game's own push lands on player 2's camera (the game's path,
// run for the right player). Ticks and rows are counted and logged.
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::location_rows {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: the counts, every 10 s while they change.
void Poll() noexcept;

/// Each engine tick: player 2's camera follows the location volume player 2's own character is in.
void Tick() noexcept;

}  // namespace lss::player_two::location_rows
