#pragma once

// Player 2's content in player 2's HUD image (Stage B2): around player 2's overlay pass
// (hud/second_image.h), the game's own answers to "which player" are switched to player 2, and the
// game's own view-dependent HUD updates are run again for player 2 (RE-p2-hud.md P8.6, pass 9 and
// pass 12). After the pass everything is switched back and the same updates run again for
// player 1, whose render follows in the same frame.
//
// The levers, each the game's own:
//   L1  each HUD root's owning player: UUserWidget::SetPlayerContext, which the marker list and
//       the world-anchored widgets read to choose the controller they project with;
//   L2  the overlay's "first game player" (the local player whose view rect it lays the HUD out in
//       and caches for the marker math): GEngine__GetFirstGamePlayerForGameViewport answers player 2
//       while the pass runs (hud/first_player_code.h);
//   L3  the party manager's primary controller (player_two/primary_controller.h Scope), which the
//       camera-manager helpers and the compass heading read;
//   L6  the game mode's HUD time step (AEndGameMode+0x5D8), 0 during the re-runs, so timers do not
//       advance twice (the overlay's own DeltaTime is L5, in hud/second_image.cpp).
// The updates run again (pass 9's re-runnable set, in the game's order): the lists at
// EndUISystem+0x98, +0xB0, +0x150 (markers), +0x188, +0x170, +0x180, and the leader-target icon at
// +0x140, and the compass holder's update (its heading follows L3). The marker math reads the
// overlay's cached view rect, so player 2's run sees player 2's rect as the previous frame's
// player-2 pass computed it, and player 1's rect is put back after.
//
// Not covered here: the compass's quest icons and the area icons (AreaMapIcon_FrameUpdate cannot run
// twice; player 2 gets their own icon manager in step B3), and player 2's command menu.

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallPlayerTwoPass(const platform::GameImage& image) noexcept;
void RemovePlayerTwoPass() noexcept;
bool PlayerTwoPassInstalled() noexcept;

/// The `hud-levers` switch: player 2's image shows player 2's content (on from a successful install).
void SetPlayerTwoContent(bool on) noexcept;
bool PlayerTwoContentOn() noexcept;

/// Diagnosis: which levers act (bits: 1 contexts, 2 first player, 4 primary controller, 8 view rect,
/// 16 updates, 32 compass, 64 the arrows' own timers, 128 the enemy bars per view, 256 the battle popups per
/// view). All at start.
void SetPlayerTwoLeversMask(unsigned mask) noexcept;

/// Game thread, just before player 2's overlay render. True if the levers are now player 2's
/// (then LeavePlayerTwo must follow the render).
bool EnterPlayerTwo() noexcept;

/// Game thread, just after player 2's overlay render: levers back to player 1, player 1's re-run.
void LeavePlayerTwo() noexcept;

}  // namespace lss::hud
