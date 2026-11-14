#pragma once

// The low-health red edge in each player's own view. Left alone, one member at a quarter of their HP or below lights the
// edge in every view: the game decides once for the whole party and draws one screen filter every view reads
// (research/RE-low-health-effect.md).
//
// The decision, by the game's own function: each command controller's tick runs the game's danger check, which reads
// each member's HP through one call. While a seat's controller decides, that call answers only for the seat's own
// member (every other member reads as never low); while player 1's decides, for every member no seat plays, so player
// 1's view also takes an AI member's danger, as in single player. Each controller's own danger value then says "my
// player is in danger". A seat's controller ticks only in a fight, so the mod runs the same check on each playing
// seat's controller once a frame (each has its own DyingStone widget the check needs), with player 1's allowed byte
// copied onto it first.
//
// The filter: the game's on and off are one latch per filter type, so a second controller's off would clear the first's.
// A seat's controller holds the filter by a count of its own instead (the manager's own count, as the game's other
// menus share a filter and as battle/menu_tint holds the tint), given back when its decision turns it off and when the
// seat stops playing. Player 1's controller keeps the game's latch. The shared filter is up while any player holds it.
//
// The fade, by the game's own code: the game fades the edge in and out with a linear fade it runs every HUD tick on the
// screen-filter slot. Each player has a zeroed slot of their own (a copy only fades: it reaches no view), set going the
// way the game's setter sets its own whenever their target changes, and run by the game's same tick function right
// after the game's own, on the same frame time.
//
// The view: the slot reaches the views through one entry of the world's screen-effect registry, applied to each view
// by the entry's own apply function. That function is replaced: for a player's view, the game's function is given a
// copy of the entry whose intensity is that player's own fade, never above the game's value; the entry itself is never
// written, so there is nothing to put back. So each view fades on its own player's edges, and every gate of the game's
// (a pause menu's fade-out and its return) still reaches every view through the cap. With one view nothing is touched.
//
// An event that hides the edge writes player 1's controller only; every player's fade follows player 1's allowed byte.
// Stone (the white filter) is decided alike by every controller and passes as the game's. Switch `low-health-per-view`
// (on: each view its own; off: the game's whole-party edge in every view). Logged: Info on each player's danger edge;
// Debug when a view first differs from the game's value and when it matches again; the `hud.low-health` reading.
// Game thread only.

#include "players/seats.h"

#include <array>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::low_health {

/// Checks the functions and call sites against the research, replaces the apply function, and places the calls (from
/// the game thread on a build whose sites cross a block). False (logged) if a check fails; nothing is placed then.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick, after the session's: which seats play a member (`plays`, by seat; index 0 unused) and whether
/// the screen is split. Each playing seat's decision (a fight's pass makes it again); a seat that stopped playing gives
/// back its filter.
void Tick(const std::array<bool, players::kMaxPlayers>& plays, bool splitShown) noexcept;

#if LSS_DEV
void RegisterCommands();
#endif

}  // namespace lss::hud::low_health
