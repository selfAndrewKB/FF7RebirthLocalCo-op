#pragma once

// Each further seat's content in its HUD image (Stage B2; per seat since plan/seats.md step 4): around the seat's
// overlay pass (hud/passes/seat_hud_images.h), the game's own answers to "which player" are switched to the seat, and
// the game's own view-dependent HUD updates are run again for it (RE-p2-hud.md P8.6, pass 9 and pass 12). After the
// pass everything is switched back and the same updates run again for player 1, whose render follows in the same frame;
// between two further passes the roots' contexts and player 1's re-run are left to the last one (hud/
// context_owners.h). Each seat keeps its own view rect between frames.
//
// The levers, each the game's own:
//   L1  each HUD root's owning player: UUserWidget::SetPlayerContext, which the marker list and
//       the world-anchored widgets read to choose the controller they project with;
//   L2  the overlay's "first game player" (the local player whose view rect it lays the HUD out in
//       and caches for the marker math): GEngine__GetFirstGamePlayerForGameViewport answers the seat's
//       local player while the pass runs (hud/passes/first_player_code.h);
//   L3  the party manager's primary controller (scopes/primary_controller.h Scope), which the
//       camera-manager helpers and the compass heading read;
//   L6  the game mode's HUD time step (AEndGameMode+0x5D8), 0 during the re-runs, so timers do not
//       advance twice (the overlay's own DeltaTime is L5, in hud/passes/seat_hud_images.cpp).
// The updates run again (pass 9's re-runnable set, in the game's order): the lists at
// EndUISystem+0x98, +0xB0, +0x150 (markers), +0x188, +0x170, +0x180, and the leader-target icon at
// +0x140, and the compass holder's update (its heading follows L3). The marker math reads the
// overlay's cached view rect, so a seat's run sees the seat's rect as its previous frame's pass computed it, and
// player 1's rect is put back after.
//
// Not covered here: the compass's quest icons and the area icons (AreaMapIcon_FrameUpdate cannot run
// twice; player 2 gets their own icon manager in step B3), and player 2's command menu.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallSeatHudContent(const platform::GameImage& image) noexcept;
void RemoveSeatHudContent() noexcept;
bool SeatHudContentInstalled() noexcept;

/// The `hud-levers` switch: each further player's image shows their own content (on from a successful install).
void SetSeatHudContent(bool on) noexcept;
bool SeatHudContentOn() noexcept;

/// Diagnosis: which levers act (bits: 1 contexts, 2 first player, 4 primary controller, 8 view rect,
/// 16 updates, 32 compass, 64 the arrows' own timers, 128 the enemy gauges per view, 256 the battle popups per
/// view). All at start.
void SetSeatLeversMask(unsigned mask) noexcept;
unsigned SeatLeversMask() noexcept;

/// Game thread, once a frame before the further passes: the roots' rescan is checked by the frame's first pass.
void BeginPasses() noexcept;

/// Game thread, just before a further seat's overlay render. True if the levers are now the seat's
/// (then LeaveSeat must follow the render). False: player 1's levers and re-run (SettleSeats), so the pass
/// renders player 1's content.
bool EnterSeat(int seat) noexcept;

/// Game thread, just after the seat's overlay render: levers back to player 1, player 1's re-run. With `another`
/// (a further seat's pass follows in this frame) the HUD roots keep the seat's contexts and player 1's re-run waits:
/// the next pass sets its own and does the way back.
void LeaveSeat(int seat, bool another) noexcept;

/// Game thread: player 1's contexts and re-run, if a pass left them for the next one (PlayerOneDue). From EnterSeat
/// when the levers fail, and after the frame's further passes when a planned pass did not run (its image not ready;
/// under seat_hud_images' HoldFrame there). Nothing otherwise.
void SettleSeats() noexcept;

/// A pass left the roots' contexts and player 1's re-run for the next further pass.
bool PlayerOneDue() noexcept;

#if LSS_DEV
/// The `hud-levers` and `hud-levers-mask` commands and readings. May throw (out of memory).
void RegisterSeatHudContentCommands();
#endif

}  // namespace lss::hud
