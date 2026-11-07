#pragma once

// The game's window spanned across a row of monitors, through the engine's own window object.
//
// One full view per monitor needs the window to cover the row (views/monitor_row.h) and the engine to lay the
// columns out (the layout tables: two players side by side, three columns for three local players; views/columns.h).
// The engine moves and sizes its window through one lever at the bottom of every path (the options menu, r.SetRes, a
// mode switch): FGenericWindow::ReshapeWindow(X, Y, Width, Height) on the game window's native window
// (FWindowsWindow, WindowsWindow.cpp:411: GetWindowInfo, the OS border added if the window has one, SetWindowPos; no
// clamp). This module calls that virtual on that object: UGameEngine::GameViewportWindow (the SWindow) ->
// SWindow::NativeWindow -> the vtable the research found (RE-game-window-reshape.md). The engine then follows its
// own window as it follows an outside resize: WM_SIZE -> FSlateApplication::OnSizeChanged -> the viewport takes the
// draw size at its next draw (SceneViewport.cpp:360, in every window mode).
//
// In the game's usual windowed fullscreen the window is already borderless, so nothing else changes: no console
// resolution, no mode switch, and the engine saves nothing into GameUserSettings (OnViewportResized saves in the
// Windowed mode only). In the Windowed mode the border stays (the engine's own rule); exclusive fullscreen refuses.
//
// Two ways in:
// - Ctrl+F2 / `window-span on` with a seat in: one monitor per column shown, the window following the
//   columns (two with player 2, three under `view-columns 3`), and left when one column is left (every seat out).
// - The three-player monitor mode, Ctrl+F3 / `multi-monitor on` (plan/seats.md step 5; the user's columns
//   rule): the span held over a row of three monitors whatever the players, so the mirror views
//   (views/mirror_player.h) fill the columns no player has. Needs a row of three; left by the same key. The settings
//   file's `multi-monitor = on` (platform/settings.h) holds it from the game's first steady frame.
// A row monitor a few pixels off the row's top (at most monitor_row::kAlignTolerance) is lined up first, for this
// Windows session only, and put back at the leave (views/display_align, taken from Remake). Left by the command,
// the key, and at the mod's end: the window goes back to the rect it had. While in, the rect is checked twice a
// second and put back if anything moved it (logged with what was seen). No display mode or scaling is ever changed:
// a row that does not qualify is refused with the reason and what to change.
// Game thread only. Logged at Info on each change, at Debug for the status.

#include "platform/image.h"

#include <cstdint>
#include <string>

namespace lss::views::window_span {

/// Verifies the window chain's code and offsets in the running build. False (logged) if they are not as researched.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Spans the game window over `columns` monitors of a row (after a line-up, a tick or more later). False (logged) if
/// it cannot.
bool Enter(std::int32_t columns) noexcept;

/// Puts the window and any lined-up monitor back; ends the monitor mode. `why` is logged. Nothing happens if not in.
void Leave(const wchar_t* why) noexcept;

/// The three-player monitor mode on (the span entered over three monitors, or re-shaped to them, and held whatever
/// the players) or off (the span left). False (logged) when it cannot be switched on.
bool Hold(bool on) noexcept;

/// The monitor mode is on.
bool Held() noexcept;

/// The settings file's multi-monitor word applied (platform/settings.h kMultiMonitor; `why` names the source for the
/// log): on = the three-player monitor mode held from the game's first steady frame (the window unchanged for half a
/// second, read from the tick), as Ctrl+F3 does, or now if the span is already in; off = nothing, and the mode left if
/// it is on. Any other word is refused with a warning.
void SetPreset(const std::wstring& word, const wchar_t* why) noexcept;

/// The multi-monitor option's word as the live state reads: on while the three-player monitor mode is held or its
/// try from the file is pending, else off.
std::wstring Preset();

/// The columns the mode holds (views/columns.h Wanted's `held`): three while on, else 0.
std::int32_t HeldColumns() noexcept;

/// Each tick, with the columns the frame shows (views/columns.h): leaves when one is left and the mode is off,
/// re-shapes the window when the count changed, finishes a line-up, holds the rect; the Debug status.
void Tick(std::int32_t columns) noexcept;

/// The span is in (or lining up).
bool In() noexcept;

/// The `window-span.state` reading.
std::wstring Reading();

}  // namespace lss::views::window_span
