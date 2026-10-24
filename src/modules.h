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
// TickAfterSession(Frame)); the entry point keeps the keys, the session, the single view's choice, the view points'
// frame, the held settings and the HUD placement between and after them. The actions a key and a control command
// share (leave, own camera, fill, third view, window span) are here too, each once.

#include "platform/dev_switch.h"
#include "players/seats.h"

#include <array>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two {
class Session;
}

namespace lss::players {
class SingleView;
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
/// session and the single view. (The shared ride is read before the session's tick, by the session itself.)
struct Frame
{
    struct Seat
    {
        bool in          = false;  // the session holds the seat's player
        bool playsMember = false;  // the seat's player plays a party member
    };
    std::array<Seat, players::kMaxPlayers> seats{};  // by seat (players/seats.h); index 0, player 1's, unused
    bool splitShown = false;  // player 2 in and the screen split (not player 1's single view)
};

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

/// Player 2 leaves: the third view first (it borrows player 2's join), then the session's leave of player 2's seat
/// (logged there).
void LeavePlayerTwo(player_two::Session& session, const wchar_t* why) noexcept;

/// Player 2's half on player 2's own camera or player 1's (views/player_view_point.h); refused without the
/// view-point entry or player 2.
Result SetOwnCamera(const player_two::Session& session, bool on) noexcept;

/// The split halves filling the screen, or kept 16:9 with bars; refused without the view-point entry.
Result SetFill(bool on) noexcept;

/// The third full-size view in or out (views/third_view.h); `why` is logged by its leave.
Result SetThirdView(bool on, const wchar_t* why) noexcept;

/// The game window across its row of monitors or back (views/window_span.h); entering needs player 2.
Result SetWindowSpan(const player_two::Session& session, bool on, const wchar_t* why) noexcept;

#if LSS_DEV
/// Every control command (platform/command_registry.h), each owner's in turn, the shared actions' here: the one list,
/// as the table above is for the installs. Game thread, once, before the console opens (control/remote.h).
void RegisterCommands(player_two::Session& session, players::SplitLayout& layout,
                      players::SingleView& singleView) noexcept;
#endif

}  // namespace lss::modules
