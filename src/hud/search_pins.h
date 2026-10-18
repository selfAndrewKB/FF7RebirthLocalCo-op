#pragma once

// The field search pins in each half: placed from that half's camera, and only that half's player's own
// (RE-p2-ranged-reticle.md; RE-p2-world-markers.md row 10).
//
// A leader-action component (PC+0x598) shows a search pin for each active interact point of its own: the ranged
// lock-on on a breakable box (`FieldShootingLockOn`, Barret and Yuffie) and the hookshot points. Each pin is a
// FieldTargetSearchIconWidget plus a hook icon in one list (EndUISystem[55], +0x1B8), and the first active point
// opens the whole-screen search compass. The list's tick runs once, in the world tick, and places every pin through
// its widget's context (player 1's), so player 2's half drew player 1's pins at player 1's screen spots (the user,
// 2026-10-18: "It leaks from P1's view to P2").
//
// Two levers, both the game's own:
// - Place: each half's pass calls the game's per-entry placement (EndUI__M1B8_PlacePin_OwnerPC_EdgeFade, then
//   EndUI__M1B8_PlaceHookIcon_ByAngle; stateless: position and render angle only) for every pin, through the
//   context the pass gave the roots: player 2's in player 2's pass, player 1's again after it. The list's tick,
//   which also runs the compass and the pins' animations, is never run again.
// - ShowOnly: before each half's render, the other player's pins (a pin is a player's when that player's component
//   holds it in an interact point, +0x1490) get render opacity 0 through the game's setter, and their own back
//   after; pins no component holds (encounter pins and other owners) stay in both halves. The compass shows in the
//   halves of the players whose component had an active point when it was last open (both when neither did).
// Logged at Debug on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::search_pins {

/// Finds the pin list and the placement functions. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Every pin and hook icon made since the last root scan becomes a HUD root at once, so this frame's context swap
/// reaches it. Before SetContexts(1).
void Adopt() noexcept;

/// Every pin placed again through its current context (after SetContexts, in each half's pass).
void Place() noexcept;

/// Hides, for the render pass about to run, the pins that are the other player's (`player` 0 or 1), and the compass
/// if it is not this player's. Restores first.
void ShowOnly(int player) noexcept;

/// Every pin and compass ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::hud::search_pins
