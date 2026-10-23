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
// frame, the held settings and the HUD placement between and after them.

namespace lss::platform {
class GameImage;
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
    bool playerTwoIn;  // the session holds player 2
    bool splitShown;   // player 2 in and the screen split (not player 1's single view)
    bool playsMember;  // player 2 plays a party member
};

/// Game thread, once per engine tick, first: the tick's identities, then each module's work that runs before the
/// keys and the session's tick (status lines, lists, the listeners, the deferred placements' polls).
void TickBeforeSession() noexcept;

/// Game thread, once per engine tick, after the session's tick and the single view's choice: each module's work
/// that reads the frame. The view points, the held settings and the HUD placement follow in the entry point.
void TickAfterSession(const Frame& frame) noexcept;

}  // namespace lss::modules
