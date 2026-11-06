#pragma once

// Each further player's own battle command menu: another FieldCommandCtrl (RE-p2-command-menu.md §5.1, §10;
// RE-p2-command-menu-input.md §4.1, §10 Stage 3a; passes S-CMD1, S-CMD2).
//
// The game owns one command controller (EndUISystem[23]) and builds it with three plain calls:
// FMemory_Malloc(0x798), FieldCommandCtrl__Ctor, FieldCommandCtrl__SetupMenuWidgets(ctrl, world). The
// ctor registers the controller with the game's GC-referenced list and Setup creates its own widgets,
// states and request arrays. A seat's controller is built the same way, once. Like the game's, it is
// never freed. It follows the game's lifecycle at the game's own calls (RE-p2-command-menu-levers.md §2):
// the one call of ResetAll and the one call of Setup on the game's controller, which every path reaches
// (field entry and exit, the fade-deferred Setup, the Blueprint thunks, the menu-close re-Setup), are
// redirected; each is made again on each seat's controller while that seat plays. When a seat joins, or
// was out at the game's Setup, a join Setup runs once the game's controller is set up.
// Setup makes focus calls (a list selection; a layer push when battle input is live), so every call on
// a seat's controller runs inside that seat's focus scope (battle/focus_scope.h) and never touches
// player 1's focus.
//
// Its menu widget stays at render opacity 0 until it is shown in its seat's pass only (command_menu_view).
// Game thread only.
//
// Units: this file's lifecycle (command_menu.cpp), the shortcut tables' follow (shortcut_follow), the widgets
// per half (command_menu_view), and what they share (command_menu_shared.h).
//
// Per seat: every seat gets its own controller and key guide, built at its first join
// and kept (indexed by the seat; index 0 unused, player 1's is the game's own). Each of the game's calls is made
// again on every built seat's controller in seat order, each in its own scopes, closed before the next seat's.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, for every built seat that plays: builds the seat's controller, runs the join Setup when it does
/// not follow the game's current setup, and logs its bytes on change.
void Tick() noexcept;

/// `seat`'s key guide (another KeyGuide53), or null if it could not be built.
void* KeyGuide(int seat) noexcept;

/// `seat`'s controller, or null before it is built.
void* Controller(int seat) noexcept;

/// The seat whose controller `controller` is, or -1 (player 1's, the game's, is no seat's): "ours" is any seat's.
int SeatOfController(const void* controller) noexcept;

}  // namespace lss::battle::command_menu
