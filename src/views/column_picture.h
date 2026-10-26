#pragma once

// What each column shows, decided in one place each tick (plan/seats.md step 6a, the user's word of 2026-10-26: the
// one-monitor and multi-monitor code laid out logically). Pure rules over this tick's facts, host-tested
// (tests/column_picture_test.cpp); the entry point (mod.cpp) applies the result and nothing else decides.
//
// A column (views/columns.h) shows one of three things:
//   - the player's own view: their own camera (views/player_view_point.h) and their own HUD (hud/passes/second_image.h);
//   - player 1's picture: player 1's camera and HUD, as a mirror view shows it (views/mirror_player.h): the further
//     player's view point is redirected to player 1's camera and no HUD pass runs for them;
//   - or the whole screen is the engine's single view, player 1's alone (players/single_view.h: the engine's split
//     switch), with no columns at all. One monitor only: a held column is never black (the user's columns rule,
//     2026-10-23), so under the three-player monitor mode or the console's `view-columns` the same moments keep the
//     columns and show player 1's picture in them instead.
//
// The moments that take a further player's view away from them, each a game state the mod reads, never a guess:
//   - the game shows something that takes the whole screen (views/whole_screen_reason.h: a full-screen menu, the map,
//     a cutscene, a minigame that holds player 1, the title) — every further player's column;
//   - player 1 rides a vehicle the whole party shares (player_two/rides/shared_ride.h) — every further player's;
//   - a further player is in without their member while the game has it (players/seat_state.h Handed: waiting for a
//     party member, or standing aside for the ride) — that player's column alone; while every further player in is
//     handed, nobody but player 1 plays, so one monitor shows the engine's single view (the user's decision, 2026-10-01);
//   - the console's `single-view on` (always player 1's) and `single-view off` (never: the development overrides).
// A column no seat fills (a mirror, a local player the mod does not know) shows player 1's picture by its nature.
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

namespace lss::views::column_picture {

/// What a column shows.
enum class Picture : std::uint8_t
{
    Own,        // the player's own camera and HUD
    PlayerOne,  // player 1's camera and HUD (as a mirror view)
};

/// Why a column shows player 1's picture (None: it shows the player's own).
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
    Single,  // always player 1's while a further player is in (development)
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

/// One further player's column, with its reason.
struct SeatPicture
{
    Picture picture = Picture::Own;
    Why     why     = Why::None;

    bool operator==(const SeatPicture&) const noexcept = default;
};

/// What the screen shows this tick.
struct Shown
{
    /// The engine's single view: one view, player 1's, no columns (one monitor only). The pictures below are then
    /// what the columns would show and do not apply.
    bool single = false;
    std::array<bool, players::kMaxPlayers> seatIn{};  // each seat in the game this tick (index 0 unused)
    /// By seat (index 0, player 1's, is always Own); a seat out of the game keeps Own with no reason.
    std::array<SeatPicture, players::kMaxPlayers> seats{};
    /// By column (views/columns.h): the column's seat's picture, player 1's picture for a column no seat fills.
    std::array<SeatPicture, views::columns::kMaxColumns> columns{};
    int columnCount = 1;  // the columns listed

    /// A further player's own view is on screen: some further seat in the game shows their own picture, not under
    /// the engine's single view. The split's per-player presentation (the HUD images, the halves' shape) runs then.
    constexpr bool SplitShown() const noexcept
    {
        if (single)
        {
            return false;
        }
        for (int seat = 1; seat < players::kMaxPlayers; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (seatIn[at] && seats[at].picture == Picture::Own)
            {
                return true;
            }
        }
        return false;
    }

    /// More than one view on screen (two or three columns, not the engine's single view): the HUD is laid out for
    /// player 1's column then.
    constexpr bool ViewsSplit() const noexcept { return columnCount > 1 && !single; }

    /// Whether `seat`'s view shows player 1's picture (false for player 1, a seat out, or under the single view).
    constexpr bool OnPlayerOne(int seat) const noexcept
    {
        return !single && players::SeatIndex(seat) &&
               seats[static_cast<std::size_t>(seat)].picture == Picture::PlayerOne;
    }

    /// The column the sound's second listener follows: the first further player listed whose view is their own, or
    /// -1 when none is (a column on player 1's picture hears as player 1's view, as a mirror's does). Under the
    /// engine's single view the first further player listed, as before step 6a: the engine lays the further views out
    /// at no size and keeps one listener itself, so the mod's slot stays where it was.
    constexpr int ListenerColumn() const noexcept
    {
        for (int column = 1; column < columnCount; ++column)
        {
            const auto& c = columns[static_cast<std::size_t>(column)];
            if (c.picture == Picture::Own || (single && c.why != Why::NoPlayer))
            {
                return column;
            }
        }
        return -1;
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
    bool furtherIn = false;
    bool allHanded = true;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        const bool in = players::InGame(moment.seats[at]);
        shown.seatIn[at] = in;
        if (in)
        {
            furtherIn = true;
            allHanded = allHanded && moment.seats[at] == players::SeatState::Handed;
        }
    }
    // Every further player's column, or the whole screen: the console's word, else the game's whole-screen state,
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
    else if (automatic && furtherIn && allHanded)
    {
        all = Why::Handed;
    }
    // One monitor: the engine's single view for those moments. Columns held: the columns stay and show player 1's
    // picture (a held column is never black).
    shown.single = furtherIn && !moment.columnsHeld && all != Why::None;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (!shown.seatIn[at])
        {
            continue;
        }
        if (all != Why::None)
        {
            shown.seats[at] = {Picture::PlayerOne, all};
        }
        else if (automatic && moment.seats[at] == players::SeatState::Handed)
        {
            shown.seats[at] = {Picture::PlayerOne, Why::Handed};  // this player alone: the others keep their views
        }
    }
    for (int column = 0; column < shown.columnCount; ++column)
    {
        const auto at   = static_cast<std::size_t>(column);
        const int  seat = moment.map.seatAt[at];
        if (seat == 0)
        {
            shown.columns[at] = {Picture::Own, Why::None};
        }
        else if (players::SeatIndex(seat) && shown.seatIn[static_cast<std::size_t>(seat)])
        {
            shown.columns[at] = shown.seats[static_cast<std::size_t>(seat)];
        }
        else
        {
            shown.columns[at] = {Picture::PlayerOne, Why::NoPlayer};
        }
    }
    return shown;
}

/// For the log: `seat`'s picture as it changed (empty while the same); `single`: the whole screen is the engine's
/// single view now (the further view is not on screen at all). A seat's change is logged by the entry point once per
/// change, for a seat in the game; the engine's single view logs its own switch (players/single_view.h).
std::wstring Describe(int seat, const SeatPicture& before, const SeatPicture& after, bool single);

/// The reason's words.
const wchar_t* NameOf(Why why) noexcept;

#if LSS_DEV
/// The `single-view` command and reading (auto, on, off): the console's choice, applied at the next tick.
void RegisterCommands(Override& choice);
#endif

}  // namespace lss::views::column_picture
