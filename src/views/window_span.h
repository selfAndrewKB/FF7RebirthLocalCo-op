#pragma once

// The game's window spanned across a row of monitors, one full view per monitor, through the engine's own window object.
//
// One full view per monitor needs the window to cover the row and the engine to lay the columns out (the layout
// tables: two players side by side, three columns for three local players; views/columns.h). The window is moved and
// sized by the engine's own reshape (views/game_window), which the engine follows as it follows an outside resize;
// Rebirth sizes its render image from the window, so nothing else changes: no console resolution, no mode switch.
//
// Two ways in:
// - Ctrl+F2 / `window-span on` with a seat in: one monitor per column shown, the window following the
//   columns (two with player 2, three under `view-columns 3`), and left when one column is left (every seat out).
// - The three-player monitor mode, Ctrl+F3 / `multi-monitor on` (the columns rule): the span held over the
//   row whatever the players, so the mirror views (views/mirror_player.h) fill the columns no player has. The
//   settings file's `multi-monitor = on` (platform/settings.h) holds it from the game's first steady frame. The mode
//   is a standing wish: refused or lost to the desktop, it is tried again at each later change of the desktop; the
//   key or the command ends it.
//
// The row: the player's monitors (the setup app, `monitors` in the settings file) need not be a row in Windows. They
// are moved into one, in the order picked, for as long as the span lasts, and put back at the leave
// (views/display_align, with a record on disk and a guard process for a crash); with none picked, a row the desktop
// has serves, its monitors a few pixels off lined up the same way (views/monitor_row, views/monitor_picks). A picked
// monitor that is not connected is waited for, never replaced by another: two of three connected make a row of two.
// No display mode or scaling is ever changed: a choice that cannot be a row is logged with the reason.
//
// The desktop can change under the game: a monitor is unplugged, a streaming host makes or removes a virtual monitor,
// Windows goes back to its saved arrangement. Twice a second the desktop's shape is compared (every monitor's rect, a
// read too cheap to feel); only on a change, or while monitors are being moved, is the display configuration read. A
// row that no longer stands is left, and the held mode comes back on its own when it stands again. While in, the
// window's rect is checked twice a second and put back if anything moved it (logged with what was seen).
// Game thread only. Logged at Info on each change, at Debug for the status.

#include "platform/image.h"

#include <cstdint>
#include <span>
#include <string>

namespace lss::views::window_span {

/// Verifies the window chain's code and offsets in the running build (views/game_window). False (logged) if they are
/// not as researched.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Spans the game window over `columns` monitors of the row (after the monitors' moves, a tick or more later). As
/// many columns as the row gives, up to `columns`. False (logged) if it cannot.
bool Enter(std::int32_t columns) noexcept;

/// Puts the window and the moved monitors back; ends the monitor mode. `why` is logged. Nothing happens if not in.
void Leave(const wchar_t* why) noexcept;

/// The three-player monitor mode on (the span entered over the row, or re-shaped to it, and held whatever the
/// players; with no row now, the mode waits for one) or off (the span left). False (logged) when the mode could not
/// span now. Switched on while the engine refuses a local player (views::LocalPlayerRefused: the pause-family menu),
/// the mode waits for that to pass and spans then (WaitsForMenu): the row's mirror views could not fill its columns
/// before, and the menu would stretch over the whole row meanwhile.
bool Hold(bool on) noexcept;

/// The monitor mode is on (spanned, or waiting for its row); not while it waits for the menu to close (WaitsForMenu).
bool Held() noexcept;

/// The monitor mode was switched on in the pause-family menu and spans once it closes.
bool WaitsForMenu() noexcept;

/// The settings file's multi-monitor word applied (platform/settings.h kMultiMonitor; `why` names the source for the
/// log): on = the three-player monitor mode held from the game's first steady frame (the window unchanged for half a
/// second, read from the tick), as Ctrl+F3 does, or now if the span is already in; off = nothing, and the mode left if
/// it is on. Any other word is refused with a warning.
void SetPreset(const std::wstring& word, const wchar_t* why) noexcept;

/// The multi-monitor option's word as the live state reads: on while the three-player monitor mode is held, its try
/// from the file is pending or it waits for the menu to close, else off.
std::wstring Preset();

/// The columns the mode holds (views/columns.h Wanted's `held`): the row's columns while on and spanned, else 0.
std::int32_t HeldColumns() noexcept;

/// Each tick, with the columns the frame shows (views/columns.h): switches the mode on once the menu it waits for has
/// closed, leaves when one is left and the mode is off, re-shapes the window when the count changed, finishes a move of
/// monitors, holds the rect, watches the desktop; the Debug status.
void Tick(std::int32_t columns) noexcept;

/// The span is in (or its monitors are being moved).
bool In() noexcept;

/// The span's monitors' widths, left to right, one per column (empty when out): for a column layout that gives each
/// view its own monitor's width. Game thread.
std::span<const std::int32_t> ColumnWidths() noexcept;

/// Their heights, the same way; the window is as tall as the tallest.
std::span<const std::int32_t> ColumnHeights() noexcept;

/// The `window-span.state` reading.
std::wstring Reading();

}  // namespace lss::views::window_span
