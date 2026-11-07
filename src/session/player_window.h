#pragma once

// Each player's own window: joining, and leaving, by pad (the rules: session/player_window_rules.h; the text:
// session/player_window_text.h).
//
// A pad is its player: the pad table's slot N is player N's (input/pad_table), so a person's own pad stays theirs.
// Start on a pad that plays no one (the pad table holds that Start back from the game) adds a player on that pad: the
// session's join (the Ctrl+F7 path) on the lowest seat no player holds, made before the session's tick (RunRequests)
// with the pad moved onto the new player's slot; the new player's window opens. During a menu or a scene the Start is
// remembered and joins once the field shows again. A join whose member is busy waits as Ctrl+F7's does; both are said
// on player 1's window for a while.
// View held a second on a player's pad opens that player's window, in their own view only (the game's own message
// window, hud/view_window; the engine's font, hud/view_prompt, where none can be shown). Only that pad then reads idle
// to the game; everyone else plays on. In it: A, Start, a B tap or View held again close it; B held a second takes
// player 2 or 3 out of the game (player 1 is refused, said on its window).
// The views becoming one (a scene, a menu) close every window.
// On the title screen a pad that presses a button becomes player 1's (its pad moved onto slot 0), so the person who
// starts the game plays player 1 on their own pad.
// Input comes from the pad table's listener, noted during the engine's pad poll and acted on in Tick. Every step is
// logged at Info. Ctrl+F7/F8 stay. Game thread.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::session {
class Session;
}

namespace lss::session::player_window {

bool Install() noexcept;
void Remove() noexcept;
/// The session whose players the windows join and take out. Set once after the install; game thread.
void SetSession(Session* session) noexcept;

/// The joins and leaves the windows asked for, made through the session. Once per engine tick on the game thread,
/// before the session's tick (where the keys' joins run); held while a menu or a scene covers the screen, so a B held
/// before a menu takes effect once it closes.
void RunRequests() noexcept;

/// What the windows need of this tick's views.
struct Frame
{
    int           screens = 1;      // the views listed (player 1's, the seats', mirrors)
    bool          single  = false;  // one view shows (the engine's single view: a scene, a menu)
    std::uint32_t seatIds = 1;      // the pad slots the players hold (bit N: player N+1's slot; bit 0 always)
};

/// Once per engine tick on the game thread, after the columns' tick.
void Tick(const Frame& frame) noexcept;

#if LSS_DEV
/// The `players.windows` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::session::player_window
