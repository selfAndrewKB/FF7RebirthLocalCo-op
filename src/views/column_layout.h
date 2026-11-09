#pragma once

// The view columns through the engine's own layout table (research/RE-splitscreen-layout-table.md): on a span of
// monitors of different widths each column is its own monitor's width; the screen order and the one-window layouts for
// three players.
//
// UGameViewportClient lays its local players out every redraw from its own layout table (SplitscreenInfo, filled by
// its constructor): with three local players it takes the three-player entry the settings name, which this module
// holds at the three-column one (players/held_setting.h, the setting put back when the third player leaves); with
// two, the side-by-side or top-and-bottom entry (players/split_layout.h). The stock records divide the window
// equally. While the window spans monitors of different widths (views/window_span.h), this module writes the entry's
// records to the monitors' widths (column_rules::ColumnRecords, the fractions stepped so the engine's truncation
// lands on each monitor's edge), and the stock records go back when the span ends, when the players no longer fill
// the entry, and at the mod's end. The layout code itself stays the engine's: it reads the table on every redraw
// (nothing is cached), so a write takes effect on the next frame and a restore likewise. The entry is written only
// when it holds the stock records or one of the mod's (any other content is logged once and left alone), and restored
// only while it still holds one of the mod's; a failed read or write is logged once and retried twice a second. [A
// data write into the engine's layout table, not a recreation of the layout: the research's one lever for columns of
// unequal widths.]
//
// Taken from Remake's views/column_layout, with the screen order and the one-window layouts. Changed: the engine has the
// three-column entry (UE 4.26), so the stock records compared are the equal thirds, the setting held names that entry
// through Rebirth's held-setting rule, and the one-window layouts are written into it; the two-player choice (the
// setting held) stays players/split_layout's, side by side on a span; mod.cpp passes the span's monitors' sizes and each
// list place's seat each tick. Reading `column-layout.state`. Game thread only. Logged at Info on each write and restore, at
// Debug for the status.

#include "platform/dev_switch.h"
#include "views/column_rules.h"

#include <cstdint>
#include <span>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::column_layout {

/// Finds GEngine. False (logged) if the engine global is outside the image: the columns keep the engine's equal
/// widths; the three-player setting is still held.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the columns' (modules::TickColumns): `localPlayers` the local players the engine lists
/// (player 1, the seats in, the mirror views); `spanWidths` and `spanHeights` the span's monitors' sizes, left to
/// right, one per column (empty when the window is not spanned); `seats` each list place's seat (-1 for a mirror's);
/// `columns` three columns held beyond the players (the span, or the console's `view-columns`: equal thirds). The
/// three-player setting held while three are listed; its entry written to three monitors' widths while three are listed
/// and spanned, to the one-window layout otherwise, arranged by the screen order; the two-player entries arranged by the
/// order while two are listed (the side-by-side one to two monitors' widths on a span); the stock records otherwise.
void Tick(int localPlayers, std::span<const std::int32_t> spanWidths, std::span<const std::int32_t> spanHeights,
          const column_rules::ListSeats& seats, bool columns) noexcept;

/// The screen order in force, the views laid out at the last tick and each list place's seat then (the player windows
/// move a view one screen through ChangeOrder). Game thread.
column_rules::Order     CurrentOrder() noexcept;
int                     Views() noexcept;
column_rules::ListSeats Seats() noexcept;

/// The screen order from now (a player moved their view), logged; nothing if it is the order in force.
void ChangeOrder(const column_rules::Order& order, const wchar_t* why) noexcept;

/// Puts the stock records and the setting back if the mod's are in. At the mod's end.
void Remove() noexcept;

/// The columns' state for the log and the console.
std::wstring Reading();

/// The `column-order` and `three-player-split` options, for the Co-op page and the console. May throw (out of memory);
/// modules::RegisterOptions catches.
void RegisterOptions();

#if LSS_DEV
/// The `column-layout.state` reading. May throw (out of memory); modules::RegisterCommands catches.
void RegisterCommands();
#endif

}  // namespace lss::views::column_layout
