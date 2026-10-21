#pragma once

// The game's window spanned across a row of monitors, through the engine's own window object.
//
// One full view per monitor needs the window to cover the row (views/monitor_row.h) and the engine to lay the
// players out in columns (the layout tables; the third view for three). The engine moves and sizes its window
// through one lever at the bottom of every path (the options menu, r.SetRes, a mode switch):
// FGenericWindow::ReshapeWindow(X, Y, Width, Height) on the game window's native window (FWindowsWindow,
// WindowsWindow.cpp:411: GetWindowInfo, the OS border added if the window has one, SetWindowPos; no clamp).
// This module calls that virtual on that object: UGameEngine::GameViewportWindow (the SWindow) ->
// SWindow::NativeWindow -> the vtable the research found (RE-game-window-reshape.md). The engine then follows its
// own window as it follows an outside resize: WM_SIZE -> FSlateApplication::OnSizeChanged -> the viewport takes the
// draw size at its next draw (SceneViewport.cpp:360, in every window mode).
//
// In the game's usual windowed fullscreen the window is already borderless, so nothing else changes: no console
// resolution, no mode switch, and the engine saves nothing into GameUserSettings (OnViewportResized saves in the
// Windowed mode only). In the Windowed mode the border stays (the engine's own rule); exclusive fullscreen refuses.
//
// Entered by the command / Ctrl+F2 with player 2 in; the number of columns is the number of local players. Left by
// the command, with player 2, and at the mod's end: the window goes back to the rect it had. While in, the rect is
// checked twice a second and put back if anything moved it (logged with what was seen). The display modes are
// never changed: a row that does not qualify is refused with the reason (tools/display.ps1 is the bench tool).
// Game thread only. Logged at Info on each change, at Debug for the status.

#include "platform/image.h"

#include <cstdint>

namespace lss::views::window_span {

/// Verifies the window chain's code and offsets in the running build. False (logged) if they are not as researched.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Spans the game window over `columns` monitors of its row. False (logged) if it cannot.
bool Enter(std::int32_t columns) noexcept;

/// Puts the window back where it was. `why` is logged. Nothing happens if not in.
void Leave(const wchar_t* why) noexcept;

/// Each tick: leaves with player 2; holds the rect; the Debug status.
void Tick(bool playerTwoIn) noexcept;

bool In() noexcept;

}  // namespace lss::views::window_span
