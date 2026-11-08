#pragma once

// A player's window in the game's own message window, in that player's view only (session/player_window;
// research/RE-message-window.md).
//
// The window is the game's `Com_Window_01` (Square's UEndMessageWindow), shown through the game's own script calls:
// `EndMenuAPI::ShowMessageWindow` (the game's whole sequence: create, add to the root overlay, the text, no options,
// open with its In animation and sound, listed in the manager whose field-start reset clears it), then at once the
// window's own `PopFromFocusStack`, so neither the keyboard nor any pad ever reaches it (the mod reads the pads); the
// text in the game's markup (pad icons, colours); closed by `HideMessageWindow` after the remove-after-Out byte is set,
// so it leaves the overlay when its Out animation ends. Another text closes it and shows the game's window again
// through the same call (one path; the window's child is never looked up). No address is needed: every call is
// reflected. It is a root on the one overlay, so every view's pass paints it: the pass driver makes the other players'
// windows transparent in each pass (ShowOnly, through hud/passes/pass_opacity).
// A window the game closed (its field-start reset) is noticed and reopened by the player windows. Opened only outside
// a seat's focus scope (the player windows' tick). Game thread.

#include "platform/dev_switch.h"

#include <string>

namespace lss::hud::view_window {

inline constexpr int kViews = 3;  // one window per player

/// Shows `markup` in `seat`'s window (opened if needed, its text replaced if open). False if no window could be shown
/// (the caller falls back to hud/view_prompt).
bool Show(int seat, const std::wstring& markup) noexcept;

/// Closes `seat`'s window (its Out animation plays, then it leaves the overlay).
void Close(int seat) noexcept;
void CloseAll() noexcept;

/// How many windows the game closed under the mod (a script's or the field start's reset): a change asks for a redraw.
unsigned long long ClosedByGame() noexcept;

/// No player's view: every window hidden in the pass (a copy of player 1's picture).
inline constexpr int kNoView = -2;

/// Before an overlay pass: only `seat`'s window stays visible (0 player 1's; kNoView none). After the passes: every
/// window back.
void ShowOnly(int seat) noexcept;
void RestoreAll() noexcept;

#if LSS_DEV
/// The `views.window` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::hud::view_window
