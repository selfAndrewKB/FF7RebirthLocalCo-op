#pragma once

// Each seat's own area-map icon manager (Stage B3; RE-p2-hud-icon-manager.md §4 and §8; per seat since
// plan/seats.md step 4).
//
// The on-screen map icons and the quest and party markers belong to one Square Enix object, the
// area-map icon manager (EndUISystem+0x1A0). Its frame function builds and places Slate icons on two
// canvases of its own in the world's root overlay, projecting with the world's first player
// controller. It keeps per-view state (the icon maps, range latches, fades), so it cannot simply be
// run twice. Each seat instead gets an instance of its own, built the way the game builds its own:
//   - created once, with the game's allocator and constructor, and kept for the rest of the process
//     (as the game keeps its own);
//   - ticked once per frame in the seat's overlay pass (after the HUD levers made the overlay's view
//     rect the seat's), with the real DeltaTime, by the game's own frame function;
//   - the frame function's one call for the first player controller answers the seat's controller
//     while that tick runs (a redirect of that single call site; every other caller is untouched);
//   - its one call for the range origin (the leader character by the leader's player type) answers
//     the seat's member the same way, so the seat's icons are ranged from the seat's character
//     (RE-p2-hud-targets.md §1, R1; the tick writes no shared state keyed on it);
//   - player 1's instance is written only by three game events (Enable, HideAndClear, SetFlagD1;
//     RE-p2-hud-view-levers.md §6, S-HUD31), from seven call sites. Each site is redirected to call
//     the event as before, then apply the event's instance effect to each seat's instance: +0xD0 = 1,
//     the game's own ClearAllAndDisable, +0xD1 = the flag. The events are not run twice (two of them
//     also animate one shared widget). A seat's instance takes player 1's state once per join;
//     the countdown at +0xDC has no positive writer, so it is left to the seat's own tick;
//   - each pass shows only its player's canvases (SWidget::SetVisibility: Hidden or the canvas's own
//     visibility), since every instance's canvases sit in the one overlay.
// When a seat leaves, its instance is cleared with the game's own clear and its canvases are hidden.
//
// Still the game's shared answers: the party markers follow player 1's leader type, and the compass
// and the "new marker" list are player 1's (§4 "still blocked").
// Game thread only.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::seat_area_icons {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// In a seat's overlay pass, after the HUD levers are the seat's: the seat's instance created if
/// needed, its flags copied from player 1's, and ticked by `deltaTime` seconds.
void Tick(int seat, float deltaTime) noexcept;

/// Before an overlay pass: only the seat's canvases (0 player 1's) are shown.
void ShowOnly(int seat) noexcept;

/// The seat is out: its instance cleared and its canvases hidden.
void SeatOut(int seat) noexcept;

/// Every seat is out: player 1's canvases shown, every seat's instance cleared and hidden.
void AllOut() noexcept;

/// Diagnosis: which parts act (1 each seat's instance ticked, 2 its first controller is the seat's,
/// 4 per-pass canvases, 8 ranged from the seat's member). All at start. Logged.
void SetMask(unsigned mask) noexcept;
unsigned Mask() noexcept;

#if LSS_DEV
/// The `icons-mask` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::hud::seat_area_icons
