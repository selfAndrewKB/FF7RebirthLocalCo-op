#pragma once

// Each seat's content in its HUD image: around the seat's overlay pass (hud/passes/column_hud_images.h), the game's
// own answers to "which player" are switched to the seat, and the game's own view-dependent HUD updates are run again
// for it (research/RE-p2-hud.md). After the pass everything is switched back and the same updates run
// again for player 1, whose render follows in the same frame; between two seats' passes the roots' contexts and player
// 1's re-run are left to the last one (hud/context_owners.h). Each column keeps its own marker host set between frames.
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
//       advance twice (the overlay's own DeltaTime is L5, in hud/passes/column_hud_images.cpp).
// The updates run again (the research's re-runnable set, in the game's order): the lists at
// EndUISystem+0x98, +0xB0, +0x150 (markers), +0x188, +0x170, +0x180, and the leader-target icon at
// +0x140, and the compass holder's update (its heading follows L3). The marker math reads the
// overlay's marker host set (facts/hud.h: the UI rect, the view rect, the host scale), which each render writes for
// its own view, so a column's run sees the column's set as its previous frame's pass wrote it, and player 1's set is put
// back after. Views of different shapes (three players: player 1's wide band, two narrow views) need the whole set.
//
// Not covered here: the area icons and the quest and party markers (AreaMapIcon_FrameUpdate cannot run twice; each
// further column has an icon manager of its own, hud/minimap/column_area_icons.h), and a seat's command menu
// (battle/command_menu.h).

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallSeatHudContent(const platform::GameImage& image) noexcept;
void RemoveSeatHudContent() noexcept;
bool SeatHudContentInstalled() noexcept;

/// The `hud-levers` switch: each seat's image shows its own content (on from a successful install).
void SetSeatHudContent(bool on) noexcept;
bool SeatHudContentOn() noexcept;

/// Diagnosis: which levers act (bits: 1 contexts, 2 first player, 4 primary controller, 8 the marker
/// host set, 16 updates, 32 compass, 64 the arrows' own timers, 128 the enemy gauges per view, 256 the battle popups per
/// view). All at start.
void SetSeatLeversMask(unsigned mask) noexcept;
unsigned SeatLeversMask() noexcept;

/// Game thread, once a frame before the seats' passes: the roots' rescan is checked by the frame's first pass, and
/// player 1's marker host set (player 1's render wrote it last) is saved for the way back after each pass.
void BeginPasses() noexcept;

/// Game thread, just before a further column's overlay render for the seat in it. The roots' contexts, the first
/// player and the marker host set follow the column's local player; the primary controller, the popups' projection and the
/// arrows' timers the seat. `copy`: the pass shows player 1's content (the enemy gauges and the minimap's content
/// player 1's). `seat` 0 is a mirror's column, always a copy: the primary controller stays player 1's (the mirror's is
/// held only around the chocobo wrangle's placement, which projects through it), the popups project with the
/// mirror's controller, the arrows keep the column's own timers. True if the levers are now the column's (then
/// LeaveSeat must follow the render). False: player 1's levers and re-run (SettleSeats), so the pass renders player
/// 1's content.
bool EnterSeat(int column, int seat, bool copy) noexcept;

/// The controller of the column whose levers are set (between EnterSeat and LeaveSeat), else null.
void* ColumnController() noexcept;

/// Game thread, just after the column's overlay render: levers back to player 1, player 1's re-run. With `another`
/// (another further pass follows in this frame) the HUD roots keep the column's contexts and player 1's re-run waits:
/// the next pass sets its own and does the way back.
void LeaveSeat(int column, int seat, bool another) noexcept;

/// Game thread, after a further column's render whose levers were refused (no LeaveSeat): player 1's marker host set
/// back in the overlay, as saved before the frame's passes (that render wrote its own). LeaveSeat and SettleSeats do it
/// themselves.
void PutBackPlayerOneHostSet() noexcept;

/// Game thread: player 1's contexts and re-run, if a pass left them for the next one (PlayerOneDue). From EnterSeat
/// when the levers fail, and after the seats' passes when a planned pass did not run (its image not ready;
/// under column_hud_images's HoldFrame there). Nothing otherwise.
void SettleSeats() noexcept;

/// A pass left the roots' contexts and player 1's re-run for the next seat's pass.
bool PlayerOneDue() noexcept;

#if LSS_DEV
/// The `hud-levers` and `hud-levers-mask` commands and readings. May throw (out of memory).
void RegisterSeatHudContentCommands();
#endif

}  // namespace lss::hud
