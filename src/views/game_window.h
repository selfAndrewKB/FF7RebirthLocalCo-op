#pragma once

// The game's own window, reached through the engine: the lever the span (views/window_span) pulls.
//
// The native window: GEngine -> UGameEngine::GameViewportWindow -> SWindow::NativeWindow, an FWindowsWindow. Its
// ReshapeWindow is the call at the bottom of every path the engine moves its window by (the options menu, r.SetRes,
// a mode switch): GetWindowInfo, the OS border added if the window has one, SetWindowPos, no clamp; the engine then
// follows its own window as it follows an outside resize (WM_SIZE -> Slate -> the viewport takes the draw size at its
// next draw). The engine sizes its render image from the window, so no resolution request is needed: in the game's
// usual windowed fullscreen the window is already borderless and nothing else changes; in the Windowed mode the border
// stays (the engine's own rule); exclusive fullscreen refuses. GetWindowMode and GetOSWindowHandle say what the window
// is now. Every read is guarded and the window's vtable must be the one the research found
// (research/RE-game-window-reshape.md). Game thread only. Install logs what is missing; the calls log nothing, their
// callers say what happened.

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
    void*        object = nullptr;  // the FWindowsWindow
    HWND__*      hwnd   = nullptr;
    std::int32_t mode   = -1;
};

/// The game's native window, or nothing with the reason in `why`: its handle must name a top-level window of this process.
std::optional<Window> Find(const wchar_t** why) noexcept;

/// The window's client area in screen coordinates: what ReshapeWindow takes (it adds the OS border itself), so a
/// reshape reads back as the rect asked for in every window mode.
Rect ClientRect(HWND__* hwnd) noexcept;

/// The engine's own reshape of its window to `rect`. False if the call faulted.
bool Reshape(void* window, const Rect& rect) noexcept;

// For the log.
std::wstring   RectText(const Rect& rect);
const wchar_t* ModeName(std::int32_t mode) noexcept;

}  // namespace lss::views::game_window
