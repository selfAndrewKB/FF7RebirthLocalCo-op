#pragma once

// Each player's own actions lock their own command menu (research/RE-p2-menu-forbid-per-player.md).
//
// A character's action that keeps the command menu shut (the field dodge and the other field extra actions, vaults,
// climbs, field actions, chests, interactions, rides) does not call the menu's setter: it counts itself on the game's
// on/off table's entry 11, and the table's applier, later in the field step, shuts the UI system's command controller's
// menu on the count's first lock and opens it again on its last release. Each site first asks whether the character
// is the player's, so in single player only the player's own actions lock, and the menu they lock is the player's own.
// The count is one for the whole party, though, and its applier knows only player 1's controller, so in co-op every
// player's action shut every player's menu.
//
// The table's two counting calls are taken: a lock or release of entry 11 made in player 2's or 3's own context on its
// thread (a climb, edge stop or dodge on the anim worker, or their character's tick and their scopes on the game
// thread, where the game asked whether the character is the player's) is counted for that player instead of on the
// game's count; their command menu's pass then shuts their menu with the game's setter while they hold one, as the
// applier does to player 1's (battle/command_menu_tick). Every other lock, player 1's own and the game's (fast travel,
// minigames, events), stays on the game's count as before. A release made outside any player's context while the game's
// count is empty belongs to the one player who still holds a lock (logged when two do); one made while the game's count
// holds goes to the game's; one no one holds is dropped, never given to the game's empty count. While either call is not
// ours (before both are placed, from removal on), both pass to the game's.
//
// Logged: every lock and release of entry 11 at Debug (whose, the reason, the caller, the thread, the counts); at Info a
// release no one holds, two players holding at an unattributed release, and a player's own lock held long without its
// release. Any thread (the game counts under its own lock; the players' counts are atomic).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::action_locks {

/// Takes the two counting calls (each checked; logged). False if either check fails: every lock then stays the game's.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The locks `seat`'s own actions hold on their command menu now (0: none; always 0 outside a seat). On the game thread
/// it also logs, once, a lock held long without its release.
int Held(int seat) noexcept;

/// `seat`'s player is out: the locks counted for them are forgotten (the game never held them). Game thread.
void SeatOut(int seat) noexcept;

}  // namespace lss::battle::action_locks
