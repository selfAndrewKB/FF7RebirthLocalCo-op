#pragma once

// Each player's minimap keeps the icons the other player's minimap needs (RE-p2-minimap-cost.md §9, §10).
//
// The navimap holder update walks every source icon in each run and tests its distance from that run's centre (the
// leader, player 2's member in the mod's player 2 run): in range, AreaMapIcon_Update (which creates the icon widget if
// it is missing); out of range, the remove helper, which destroys the widget and its label. With the players apart,
// every run removed the icons only the other player has in range, and the other player's next run made them again:
// ~20 icons and labels destroyed and created per frame, each label laid out twice (~5.5 ms a frame on the game thread).
//
// Here the holder's two calls are redirected:
// - its AreaMapIcon_Update call records the key: this player's run keeps that icon, by the game's own tests;
// - its remove call is skipped for a key the other player's last run kept. The kept icon stays in the holder; this
//   run's render loop hides it with its own range test (same radius, same centre), which is not touched.
// Nothing is decided by the mod: "keep" is the game's answer for the other player, remembered for one run, as the map
// pieces' unload test (hud/minimap.cpp) asks for the other player's centre. Only while player 2's minimap is on;
// otherwise both calls go straight to the game's. Optional: if a check fails, icons churn as before (logged).
// Logs the counts every two minutes (Debug), the first kept icon (Debug), and any kept icon this run shows (Info).
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap_icons {

/// Checks both call sites and their targets (hud/minimap.cpp's Install) and queues their placement for the game
/// thread, outside the holder update (their calls cross a 16-byte block). Logs a failure.
bool Prepare(const platform::GameImage& image) noexcept;

/// Puts the calls back on the game thread (else leaves them: they pass through once inactive).
void Remove() noexcept;

/// Each engine tick: whether player 2's minimap is on. Off: the records are cleared and nothing is skipped.
void Tick(bool playerTwoMinimapOn) noexcept;

/// Around the mod's holder runs (1: player 2's, 0: player 1's). The game's own call counts as player 1's centre.
void BeginRun(int run) noexcept;
void EndRun(const void* holder) noexcept;

}  // namespace lss::hud::minimap_icons
