#pragma once

// The game's own window, reached through the engine: the lever the span (views/window_span) pulls.
//
// The native window: GEngine -> UGameEngine::GameViewportWindow -> SWindow::NativeWindow, an FWindowsWindow. Its
// ReshapeWindow is the call at the bottom of every path the engine moves its window by (the options menu, r.SetRes,
// a mode switch): GetWindowInfo, the OS border added if the window has one, SetWindowPos, no clamp of its own; the
// engine then follows its own window as it follows an outside resize (WM_SIZE -> Slate -> the viewport takes the draw
// size at its next draw). The engine sizes its render image from the window, so no resolution request is needed: in
// the game's usual windowed fullscreen the window is already borderless and nothing else changes; in the Windowed mode
// the border stays (the engine's own rule); exclusive fullscreen refuses. One limit stands in the way: the OS asks the
// window for its largest size (WM_GETMINMAXINFO) and the engine answers with the SWindow's size limits, which a start
// in the Windowed mode sets to the desktop's size of that moment, so a row wider than that desktop would be cut to it;
// the span raises the limit to the row before the reshape and puts it back after (RE-window-size-limits.md).
// GetWindowMode and GetOSWindowHandle say what the window is now. Every read is guarded and the window's vtable must
// be the one the research found (research/RE-game-window-reshape.md). Game thread only. Install logs what is missing;
// the calls log nothing, their callers say what happened.

#include "views/monitor_row.h"

#include <cstdint>
#include <optional>
#include <string>

struct HWND__;

namespace lss::platform {
class GameImage;
}

namespace lss::views::game_window {

using monitor_row::Rect;

// EWindowMode, as GetWindowMode answers.
inline constexpr std::int32_t kModeFullscreen         = 0;  // exclusive
inline constexpr std::int32_t kModeWindowedFullscreen = 1;  // borderless
inline constexpr std::int32_t kModeWindowed           = 2;

/// Verifies the window chain's code and offsets in the running build. False (logged) if they are not as researched;
/// nothing else here may be called then.
bool Install(const platform::GameImage& image) noexcept;

struct Window
{
    void*        object  = nullptr;  // the FWindowsWindow
    void*        swindow = nullptr;  // the SWindow it belongs to
    HWND__*      hwnd    = nullptr;
    std::int32_t mode    = -1;
};

/// The game's native window, or nothing with the reason in `why`: its handle must name a top-level window of this process.
std::optional<Window> Find(const wchar_t** why) noexcept;

/// The engine's own limit on the window's size (SWindow::SizeLimits MaxWidth and MaxHeight, facts/engine.h): set once
/// from the desktop's size when the game starts in the Windowed mode, unset in the other modes. The OS cuts every
/// reshape to it, so a row wider than the desktop at the game's start would be cut to that desktop.
struct SizeLimit
{
    bool  set    = false;
    float width  = 0.0f;
    float height = 0.0f;
};

/// Raises the limit to at least `width` x `height` where it is set below them; `before` receives the limit as it was
/// when it changed (for RestoreSizeLimit at the leave), nothing when nothing changed. False if the limit could not be
/// read or written or does not read as the engine's (logged by the caller; the reshape is tried all the same).
bool RaiseSizeLimit(const Window& window, std::int32_t width, std::int32_t height, std::optional<SizeLimit>& before) noexcept;

/// Puts a raised limit back. False if the write faulted.
bool RestoreSizeLimit(const Window& window, const SizeLimit& before) noexcept;

/// The window's client area in screen coordinates: what ReshapeWindow takes (it adds the OS border itself), so a
/// reshape reads back as the rect asked for in every window mode.
Rect ClientRect(HWND__* hwnd) noexcept;

/// The engine's own reshape of its window to `rect`. False if the call faulted.
bool Reshape(void* window, const Rect& rect) noexcept;

// For the log.
std::wstring   RectText(const Rect& rect);
const wchar_t* ModeName(std::int32_t mode) noexcept;

}  // namespace lss::views::game_window
