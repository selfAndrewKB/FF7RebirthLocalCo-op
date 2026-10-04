#pragma once

// The game's grid level streaming follows both players, as the NPC population does (the user's choice of 2026-10-04:
// "If you can already write it similarly to how NPCs stream in, I'd rather just have that. Not too concerned with
// volumes"; RE-p2-world-streaming.md §1, §9; RE-p2-field-interact.md §14.4a).
//
// Square Enix's streaming manager loads each AEndStreamingGrid cell's sub-level while one point, the party leader's
// location, is inside the cell's load box (the cell's cooked LoadDistance around it, built once into the cell when the
// grid registers), and releases it past the cell's unload margin. The area data of chests, townspeople and creatures
// comes with those sub-levels, so near player 2 and far from player 1 they never exist. There is one point and no path
// for a second.
//
// While player 2 plays (whole-screen menus and scenes included: their surroundings stay loaded; and the NPC centre's switch
// is on, Ctrl+F10):
//   - the manager's grid update (its call 0x140A9EE49 in the streaming tick, redirected) gets the midpoint of the two
//     players' characters as its point; volumes keep the game's own point (player 1, or a camera), so an interior around
//     player 1 never unloads because of the midpoint;
//   - every live grid cell's LoadDistance is widened by half the players' separation (in 5 m steps) and the grid's load
//     boxes rebuilt with the game's own EndStreamingGrid_BuildCellLoadBoxes; the height-checked grids' height threshold
//     (+0x238, read live) by half their height difference. Every cell either player stands in then holds the midpoint.
// The cells' cooked values are put back (and the boxes rebuilt) when player 2 stops playing, the switch
// turns it off, or the game sets its own streaming override point (mgr+0x3A4, an event's; left to it until it clears).
// Logged: start, stop and defer (Info), a summary every 60 s (Debug). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::streaming_centre {

/// Finds the manager, the grid update's call and the box rebuild (each checked). Logged.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether player 2 plays a member (a whole-screen menu or scene keeps it on).
void Tick(bool playerTwoPlays) noexcept;

/// Puts the cells back (at mod uninstall; on the game thread only, else logged).
void Release() noexcept;

}  // namespace lss::player_two::streaming_centre
