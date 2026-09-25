#pragma once

// Player 2's own area-map icon manager (Stage B3; RE-p2-hud-icon-manager.md §4 and §8).
//
// The on-screen map icons and the quest and party markers belong to one Square Enix object, the
// area-map icon manager (EndUISystem+0x1A0). Its frame function builds and places Slate icons on two
// canvases of its own in the world's root overlay, projecting with the world's first player
// controller. It keeps per-view state (the icon maps, range latches, fades), so it cannot simply be
// run twice. Player 2 instead gets a second instance, built the way the game builds its own:
//   - created once, with the game's allocator and constructor, and kept for the rest of the process
//     (as the game keeps its own);
//   - ticked once per frame in player 2's overlay pass (after the HUD levers made the overlay's view
//     rect player 2's), with the real DeltaTime, by the game's own frame function;
//   - the frame function's one call for the first player controller answers player 2's controller
//     while that tick runs (a redirect of that single call site; every other caller is untouched);
//   - player 1's instance is written only by game events (enable, clear, the flag at +0xD1 and the
//     countdown at +0xDC), so each tick copies them into player 2's instance, and a disable is
//     applied with the game's own clear;
//   - each pass shows only its player's canvases (SWidget::SetVisibility: Hidden or the canvas's own
//     visibility), since both instances' canvases sit in the one overlay.
// When player 2 leaves, player 2's instance is cleared with the game's own clear and its canvases
// are hidden.
//
// Still the game's shared answers: icons are ranged from the party leader, and the compass and the
// "new marker" list are player 1's (§4 "still blocked").
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::area_icons_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// In player 2's overlay pass, after the HUD levers are player 2's: player 2's instance created if
/// needed, its flags copied from player 1's, and ticked by `deltaTime` seconds.
void Tick(float deltaTime) noexcept;

/// Before an overlay pass: only `player`'s canvases (0 = player 1, 1 = player 2) are shown.
void ShowOnly(int player) noexcept;

/// Player 2 is out: player 2's instance cleared and its canvases hidden.
void PlayerTwoOut() noexcept;

/// Diagnosis: which parts act (1 player 2's instance ticked, 2 its first controller is player 2's,
/// 4 per-pass canvases). All at start. Logged.
void SetMask(unsigned mask) noexcept;

}  // namespace lss::hud::area_icons_two
