#pragma once

// Where each column's view comes from, decided in one place each tick (plan/seats.md step 6a, the user's word of
// 2026-10-26: the one-monitor and multi-monitor code laid out logically). Pure rules over this tick's facts,
// host-tested (tests/column_source_test.cpp); the entry point (mod.cpp) applies the result and nothing else decides.
//
// A column (views/columns.h) shows one of three things, one mechanism each:
//   - Self: the player's own view, their own camera (views/player_view_point.h) and their own HUD
//     (hud/passes/column_hud_images.h);
//   - PlayerOne: the column stays and the mod puts player 1's camera and HUD in it, as a mirror view shows them
//     (views/mirror_player.h): the seat's view point is redirected to player 1's camera and no HUD pass
//     runs for them;
//   - Screen: no column at all; the whole screen is the engine's single view, player 1's (players/single_view.h: the
//     engine's split switch), and the mod does nothing per seat. One monitor only: a held column is never black (the
//     user's columns rule, 2026-10-23), so under the three-player monitor mode or the console's `view-columns` the
//     same moments keep the columns and give them PlayerOne instead.
//
// The moments that take a seat's view away from its player, each a game state the mod reads, never a guess:
//   - the game shows something that takes the whole screen (views/whole_screen_reason.h: a full-screen menu, the map,
//     a cutscene, a minigame that holds player 1, the title) — every seat's column;
//   - player 1 rides a vehicle the whole party shares (rides/shared_ride.h) — every seat's;
//   - a seat's player is in without their member while the game has it (players/seat_state.h Handed: waiting for a
//     party member, or standing aside for the ride) — that player's column alone; while every seat in the game is
//     handed, nobody but player 1 plays, so one monitor shows the engine's single view (the user's decision,
//     2026-10-01). On one monitor a player waiting for their member leaves the game after a short grace and waits
//     outside (session/session_rules.h WaitOutside; the user's decision, 2026-10-27), so their column goes
//     with them and the others' views fill the screen; with columns held they stay, their column on player 1's picture;
//   - the console's `single-view on` (always player 1's) and `single-view off` (never: the development overrides).
// A column no seat fills (a mirror, a local player the mod does not know) shows player 1's by its nature.
//
// The table moment x surface is in the plan (plan/topics/multi-monitor.md, "What each column shows"); the tests hold
// every row of it.

#include "platform/dev_switch.h"
#include "players/seat_state.h"
#include "players/seats.h"
#include "views/columns.h"

#include <array>
#include <cstdint>
#include <string>

namespace lss::views::column_source {

/// Where a column's view comes from.
enum class Source : std::uint8_t
{
    Self,       // the player's own camera and HUD
    PlayerOne,  // player 1's camera and HUD in the player's column (the redirect, no HUD pass), as a mirror view
    Screen,     // no column: the engine's single view, player 1's screen (one monitor only)
};

/// Why a column is not the player's own (None: it is).
enum class Why : std::uint8_t
{
    None,
    WholeScreen,  // the game takes the whole screen (a menu, a cutscene, a minigame, the title)
    SharedRide,   // player 1 rides the buggy or the bronco
    Handed,       // the player is in without their member while the game has it (waiting, or standing aside)
    Console,      // `single-view on`
    NoPlayer,     // no seat fills the column (a mirror view, a local player the mod does not know)
};

/// The console's override of the automatic choice (`single-view auto|on|off`).
enum class Override : std::uint8_t
{
    Auto,    // by the moments above (the default)
    Single,  // always player 1's while a seat is in (development)
    Split,   // never (development)
};

/// This tick's facts, from the session, the columns and the whole-screen read.
struct Moment
{
    bool          wholeScreen = false;  // the game shows something that takes the whole screen
    bool          sharedRide  = false;  // player 1 rides a vehicle the whole party shares
    bool          columnsHeld = false;  // columns held beyond the players: the monitor mode, or `view-columns`
    Override      choice      = Override::Auto;  // the console's `single-view`
    columns::Map  map;                  // the engine's list as columns, after the mirror views' tick
    std::array<players::SeatState, players::kMaxPlayers> seats{};  // each seat's word (index 0, player 1's, unused)
};

/// One column's source, with its reason.
struct SeatSource
{
    Source source = Source::Self;
    Why    why    = Why::None;

    bool operator==(const SeatSource&) const noexcept = default;
};

/// What the screen shows this tick.
struct Shown
{
    std::array<bool, players::kMaxPlayers> seatIn{};  // each seat in the game this tick (index 0 unused)
    /// By seat (index 0, player 1's, is always Self); a seat out of the game keeps Self with no reason.
    std::array<SeatSource, players::kMaxPlayers> seats{};
    /// By column (views/columns.h): the column's seat's source, player 1's for a column no seat fills.
    std::array<SeatSource, views::columns::kMaxColumns> columns{};
    int columnCount = 1;  // the columns listed
    /// By seat, the column showing their own view (OwnColumn), -1 for none (index 0 unused).
    std::array<int, players::kMaxPlayers> ownColumns{-1, -1, -1};

    /// The engine's single view: one view, player 1's, no columns (one monitor only): some seat in the game
    /// has Screen (every one has it then, the moments above give it to all).
    constexpr bool Single() const noexcept
    {
        for (int seat = 1; seat < players::kMaxPlayers; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (seatIn[at] && seats[at].source == Source::Screen)
            {
                return true;
            }
        }
        return false;
    }

    /// A seat's own view is on screen: some seat in the game shows its own. The split's
    /// per-player presentation (the HUD images, the halves' shape) runs then.
    constexpr bool SplitShown() const noexcept
    {
        for (int seat = 1; seat < players::kMaxPlayers; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (seatIn[at] && seats[at].source == Source::Self)
            {
                return true;
            }
        }
        return false;
    }

    /// More than one view on screen (two or three columns, not the engine's single view): the HUD is laid out for
    /// player 1's column then.
    constexpr bool ViewsSplit() const noexcept { return columnCount > 1 && !Single(); }

    /// Whether `seat`'s column shows player 1's camera and HUD by the mod (false for player 1, a seat out, and under
    /// the engine's single view, where the seat's column does not exist).
    constexpr bool OnPlayerOne(int seat) const noexcept
    {
        return players::SeatIndex(seat) && seats[static_cast<std::size_t>(seat)].source == Source::PlayerOne;
    }

    /// The column showing `seat`'s own view (Self, listed), or -1: the column whose engine listener is that player's
    /// (feedback/sound_listeners.h). None for a column on player 1's picture (it hears as player 1's view,
    /// as a mirror's does) and none under the engine's single view (one view, one listener: player 1's).
    constexpr int OwnColumn(int seat) const noexcept
    {
        return players::SeatIndex(seat) ? ownColumns[static_cast<std::size_t>(seat)] : -1;
    }

    bool operator==(const Shown&) const noexcept = default;
};

/// The decision. Pure; every row of the plan's table is a test.
constexpr Shown Decide(const Moment& moment) noexcept
{
    Shown shown;
    shown.columnCount = moment.map.count > 0 ? moment.map.count : 1;
    if (shown.columnCount > columns::kMaxColumns)
    {
        shown.columnCount = columns::kMaxColumns;  // a map built by MapOf never is; the field is open
    }
    bool anySeatIn = false;
    bool allHanded = true;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        const bool in = players::InGame(moment.seats[at]);
        shown.seatIn[at] = in;
        if (in)
        {
            anySeatIn = true;
            allHanded = allHanded && moment.seats[at] == players::SeatState::Handed;
        }
    }
    // Every seat's column, or the whole screen: the console's word, else the game's whole-screen state,
    // the shared ride, or nobody but player 1 playing.
    const bool automatic = moment.choice == Override::Auto;
    Why        all       = Why::None;
    if (moment.choice == Override::Single)
    {
        all = Why::Console;
    }
    else if (automatic && moment.wholeScreen)
    {
        all = Why::WholeScreen;
    }
    else if (automatic && moment.sharedRide)
    {
        all = Why::SharedRide;
    }
    else if (automatic && anySeatIn && allHanded)
    {
        all = Why::Handed;
    }
    // One monitor: the engine's single view for those moments. Columns held: the columns stay and show player 1's
    // camera and HUD (a held column is never black).
    const Source everyColumn = moment.columnsHeld ? Source::PlayerOne : Source::Screen;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (!shown.seatIn[at])
        {
            continue;
        }
        if (all != Why::None)
        {
            shown.seats[at] = {everyColumn, all};
        }
        else if (automatic && moment.seats[at] == players::SeatState::Handed)
        {
            shown.seats[at] = {Source::PlayerOne, Why::Handed};  // this player alone: the others keep their views
        }
    }
    for (int column = 0; column < shown.columnCount; ++column)
    {
        const auto at   = static_cast<std::size_t>(column);
        const int  seat = moment.map.seatAt[at];
        if (column == 0 || seat == 0)  // player 1's column is their own, listed or not (an empty map)
        {
            shown.columns[at] = {Source::Self, Why::None};
        }
        else if (players::SeatIndex(seat) && shown.seatIn[static_cast<std::size_t>(seat)])
        {
            shown.columns[at] = shown.seats[static_cast<std::size_t>(seat)];
        }
        else
        {
            shown.columns[at] = {Source::PlayerOne, Why::NoPlayer};
        }
    }
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const auto at     = static_cast<std::size_t>(seat);
        const int  column = moment.map.columnOf[at];
        if (shown.seatIn[at] && shown.seats[at].source == Source::Self && column >= 1 && column < shown.columnCount)
        {
            shown.ownColumns[at] = column;
        }
    }
    return shown;
}

/// For the log: `seat`'s source as it changed (empty while the same). A seat's change is logged by the entry point
/// once per change, for a seat in the game; the engine's single view logs its own switch (players/single_view.h).
std::wstring Describe(int seat, const SeatSource& before, const SeatSource& after);

/// The reason's words.
const wchar_t* NameOf(Why why) noexcept;

#if LSS_DEV
/// The `single-view` command and reading (auto, on, off): the console's choice, applied at the next tick.
void RegisterCommands(Override& choice);
#endif

}  // namespace lss::views::column_source