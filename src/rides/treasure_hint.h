#pragma once

// The treasure hint for every rider: the status icon over a rider's chocobo when a treasure, a burrow or a quest trail
// is near, before any press, in that rider's own view (research/RE-chocobo-treasure-hint-seats.md).
//
// The Scour and Scent manager's tick (rides/scour_scent) decides the hint inline, for one rider: the party leader's
// chocobo, then shows it through the session's one icon. No game function answers the question for a second rider. So:
// - the tick runs as one player each frame: the session's owner; else player 1 while riding, or while a quest trail
//   is being revealed on foot (the tick's on-foot part reveals it for the walker); else the first player 2 or 3 who
//   rides; else player 1. The session's icon set belongs to that player; when the player changes, the sets trade
//   places, so no icon is rebuilt;
// - every other rider has an icon set of their own, made, shown, animated, placed and removed by the game's own
//   ChocoCraft_UpdateStatusWidgets, called in that rider's primary scope and leader answer with their set swapped into
//   the session for the call (the session's dig, sniff, discover and trail fields read idle, the dug item's widget
//   empty) and every swapped field put back after;
// - the hint's flags for those riders come from the mod's copy of the game's nearest-point test (the one recreation:
//   the game's point table, ranges, names and trail splines, read only, through the game's own spline helpers). Every
//   frame the same copy runs for the tick's player and is compared with the game's own flags, captured at the game's
//   call (a call redirect): a disagreement held for two frames is a Warning;
// - each view shows only its own player's set (render opacity through players/widget_opacity), and in a player 2's or
//   3's HUD pass their icon is placed again through the game's own placement from their view.
// Logged: Info at install, when the tick's player changes and when the game's own hint changes (its icon and point);
// Debug per rider's hint change; Warning on a disagreement. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::rides::treasure_hint {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Before the manager's tick: the player it runs as this frame (`owner`: the session's owner, -1 nobody's), with the
/// session's icon set made that player's.
int BeforeTick(void* manager, int owner) noexcept;

/// After the manager's tick ran as `tickPlayer`: the comparison with the game's own flags, then every other rider's
/// hint.
void AfterTick(void* manager, int tickPlayer) noexcept;

/// The player whose icon set the session holds (player 1 before any tick).
int SessionSetPlayer() noexcept;

/// In `seat`'s HUD pass (the pass holds the seat's primary controller): the seat's icon placed again from its view.
void Place(int seat) noexcept;

/// Hides, for the render pass about to run, every other player's own icon set (the session's set: rides/scour_scent).
/// Restores first.
void ShowOnly(int seat) noexcept;

/// Every icon ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::rides::treasure_hint
