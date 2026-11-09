#pragma once

// Each further column's own area-map icon manager (RE-p2-hud-icon-manager.md §4 and §8).
//
// The on-screen map icons and the quest and party markers belong to one Square Enix object, the
// area-map icon manager (EndUISystem+0x1A0). Its frame function builds and places Slate icons on two
// canvases of its own in the world's root overlay, projecting with the world's first player
// controller. It keeps per-view state (the icon maps, range latches, fades), so it cannot simply be
// run twice. Each further column instead gets an instance of its own, built the way the game builds its own:
//   - created once, with the game's allocator and constructor, and kept for the rest of the process
//     (as the game keeps its own);
//   - ticked once per frame in the column's overlay pass (after the HUD levers made the overlay's view
//     rect the column's), with the real DeltaTime, by the game's own frame function;
//   - the frame function's one call for the first player controller answers the column's controller
//     while that tick runs (a redirect of that single call site; every other caller is untouched);
//   - its one call for the range origin (the leader character by the leader's player type) answers
//     the shown player's member the same way (a copy of player 1's picture: the game's own answer), so
//     the icons are ranged from that player's character (RE-p2-hud-targets.md §1, R1; the tick writes no
//     shared state keyed on it);
//   - player 1's instance is written only by three game events (Enable, HideAndClear, SetFlagD1;
//     RE-p2-hud-view-levers.md §6, S-HUD31), from seven call sites. Each site is redirected to call
//     the event itself, then apply the event's instance effect to each column's instance: +0xD0 = 1,
//     the game's own ClearAllAndDisable, +0xD1 = the flag. The events are not run again (two of them
//     also animate one shared widget). A column's instance takes player 1's state when the column
//     changes hands (a join, a leave, a mirror in or out); the countdown at +0xDC has no positive writer,
//     so it is left to the column's own tick;
//   - each pass shows only its column's canvases (SWidget::SetVisibility: Hidden or the canvas's own
//     visibility), since every instance's canvases sit in the one overlay.
// When a column changes hands, its instance is cleared with the game's own clear and its canvases are hidden.
//
// Still the game's shared answers: the party markers follow player 1's leader type, and the compass
// and the "new marker" list are player 1's (§4 "still blocked").
// Game thread only.

#include "platform/dev_switch.h"
#include "views/columns.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::column_area_icons {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once a frame before the passes: a further column whose controller id changed since the last frame (a join, a
/// leave, a mirror in or out) has its instance cleared and its canvases hidden; its next tick starts from player 1's
/// event state. An id the list could not read counts as a change too (the column is started again).
void FollowColumns(const views::columns::Map& map) noexcept;

/// In a further column's overlay pass, after the HUD levers are the column's: the column's instance created if
/// needed and ticked by `deltaTime` seconds, placed through `controller` (the column's) and ranged from `member`
/// (the shown player's character; null: the game's own answer, player 1's).
void Tick(int column, void* controller, void* member, float deltaTime) noexcept;

/// Before an overlay pass: only the column's canvases (0 player 1's) are shown.
void ShowOnly(int column) noexcept;

/// The icon of `key` in the column's instance (0: player 1's, the game's) withheld from this paint: its widget set
/// Hidden as the game sets it. The instance's next tick sets it again, so it is asked after each tick, before the
/// paint. False when the key has no widget there yet (or it could not be set).
bool HideIcon(int column, std::uint64_t key) noexcept;

/// No player in: player 1's canvases shown, every column's hidden (each instance is cleared when its column changes
/// hands, FollowColumns).
void AllOut() noexcept;

/// Diagnosis: which parts act (1 each column's instance ticked, 2 its first controller is the column's,
/// 4 per-pass canvases, 8 ranged from the shown player's member). All at start. Logged.
void SetMask(unsigned mask) noexcept;
unsigned Mask() noexcept;

#if LSS_DEV
/// The `icons-mask` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::hud::column_area_icons
