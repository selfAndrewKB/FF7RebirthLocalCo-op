#pragma once

// The module table (modules.cpp): every module the entry point installs after the session, in install order, each
// with its Remove or none. The entry point (mod.cpp) installs the table in order and removes it in reverse; each
// module logs its own install line, and one whose check fails stands aside alone. The order constraints the modules
// carry (climb_context before traversal_camera, the view point before the streaming views, the overlay layout
// before the layout scale, the opacity setter before the HUD modules that read it) are pinned by static_asserts
// beside the table: the build fails if a row moves past one.
//
// Outside the table, in the entry point: what every module relies on (the renderer, the weak pointers, the
// in-battle test, the session and its stages) before it; the dev tools, the engine callbacks and the field-exit
// catch (they take the session or a callback of the mod's) after it.
//
// The same file lists the modules' per-tick calls, in two phases around the session's tick (TickBeforeSession,
// TickAfterSession(Frame)); the entry point keeps the keys, the session, the columns' sources (views/column_source.h,
// where each column's view comes from), the view points' frame, the held settings and the HUD placement between and
// after them.
// The actions a key and a control command share (leave, own camera, fill, the monitor mode, window span) are here too,
// each once. So is the columns' tick (the mirror views and the window span toward the columns the frame shows), right
// after the session's.

#include "platform/dev_switch.h"
#include "players/seat_state.h"
#include "players/seats.h"
#include "views/column_source.h"
#include "views/columns.h"

#include <array>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::session {
class Session;
}

namespace lss::players {
class SplitLayout;
}

namespace lss::modules {

/// Installs every module in table order.
void Install(const platform::GameImage& image) noexcept;

/// Removes every module that has a Remove, in reverse table order. The rows without one patch nothing
/// (charapop_centre puts its entry back in Release, before the unload's gate); their state stays, the DLL being
/// pinned.
void Remove() noexcept;

/// What the modules read once per engine tick, after the session's tick; the entry point fills it from the
/// session and the columns' sources (views/column_source.h). (The shared ride is read before the session's tick, by
/// the session itself.)
struct Frame
{
    struct Seat
    {
        players::SeatState state = players::SeatState::Out;  // the session's word for the seat (players/seat_state.h)

        /// The session holds the seat's player (in, playing or handed).
        constexpr bool In() const noexcept { return players::InGame(state); }
        /// The seat's player plays a party member.
        constexpr bool PlaysMember() const noexcept { return state == players::SeatState::Playing; }
    };
    std::array<Seat, players::kMaxPlayers> seats{};  // by seat (players/seats.h); index 0, player 1's, unused
    views::column_source::Shown shown;  // what each column shows this tick (views/column_source.h)
    bool splitShown = false;  // shown.SplitShown(): a seat in and its own view on screen
    int  columns    = 1;      // local players listed, one view each while the screen splits (views/columns.h)
};

/// The columns of one tick (views/columns.h).
struct Columns
{
    views::columns::Map map;            // the engine's list as columns, after the mirror views' tick
    int                 playersIn = 1;  // player 1 and the seats listed
    int                 wanted    = 1;  // the columns the frame shows
    bool                held      = false;  // columns held beyond the players: the monitor mode or `view-columns`
};

/// Game thread, once per engine tick right after the session's: the columns wanted (the players listed, or the ones
/// held), the mirror views toward them (views/mirror_player.h: added whenever player 1's controller and camera
/// manager are ready, through loading screens and the seats' rejoins alike, held only while the engine would refuse
/// a local player, views::LocalPlayerRefused), the window span to them (views/window_span.h).
Columns TickColumns() noexcept;

/// Before a map load (never a field exit inside the world: the mirrors stay through those, so the loading screen is
/// drawn in every column): the mirror views out with the seats, since the new map would spawn their controllers
/// again outside a join; they come back on the new map's first ticks. `why` is logged.
void ClearMirrorsForMapLoad(const wchar_t* why) noexcept;

/// Game thread, once per engine tick, first: the tick's identities, then each module's work that runs before the
/// keys and the session's tick (status lines, lists, the listeners, the deferred placements' polls).
void TickBeforeSession() noexcept;

/// Game thread, once per engine tick, after the session's tick and the single view's choice: each module's work
/// that reads the frame. The view points, the held settings and the HUD placement follow in the entry point.
void TickAfterSession(const Frame& frame) noexcept;

/// What an action did: the key logs the text, the control command returns it as its outcome.
struct Result
{
    bool         ok;
    std::wstring text;
};

// The actions a key and a control command share, each in one place. Game thread.

/// A seat with a player in it, or waiting to be (for a free member, or to rejoin after a map load).
bool SeatTaken(const session::Session& session, int seat) noexcept;

/// The seat Ctrl+F7 adds: the lowest built seat not in (a seat waiting to join is asked again), or 0 when every seat
/// is in.
int SeatToJoin(const session::Session& session) noexcept;

/// The seat Ctrl+F8 removes: the highest built seat taken, or 0 when none is.
int SeatToLeave(const session::Session& session) noexcept;

/// `seat`'s player leaves: the session's leave of that seat, which also stops a wait (logged there). A mirror view
/// takes the column once it is free, if columns are held.
void LeaveSeat(session::Session& session, int seat, const wchar_t* why) noexcept;

/// The seats' views on their own cameras or player 1's (views/player_view_point.h); refused without the
/// view-point entry or a seat in.
Result SetOwnCamera(const session::Session& session, bool on) noexcept;

/// The split halves filling the screen, or kept 16:9 with bars; refused without the view-point entry.
Result SetFill(bool on) noexcept;

/// The three-player monitor mode on or off (views/window_span.h Hold): three monitors whatever the players, the
/// mirror views in the columns no player has.
Result SetMonitorMode(bool on) noexcept;

/// The game window across its row of monitors or back (views/window_span.h); entering needs a seat in.
Result SetWindowSpan(const session::Session& session, bool on, const wchar_t* why) noexcept;

#if LSS_DEV
/// Every control command (platform/command_registry.h), each owner's in turn, the shared actions' here: the one list,
/// as the table above is for the installs. Game thread, once, before the console opens (control/remote.h).
void RegisterCommands(session::Session& session, players::SplitLayout& layout,
                      views::column_source::Override& choice) noexcept;
#endif

}  // namespace lss::modules
