#pragma once

// Who may issue commands to whom in the battle command menu (RE-p2-command-menu.md §4, pass S-CMD1).
//
// The leader commands an ally through the command menu's ally mode (LT/RT, the shortcut palette):
// the menu's chooser walks the members and skips one with no HP (knocked out) or petrified, and a
// second test (the member can take a command: has usable commands, not stopped, asleep, petrified or
// blocked) guards the strict path, the ally pick and the ally-mode portraits. The portrait greying
// alone blocks nothing. Player 1 must not command the member player 2 plays, so both levers answer
// for that member as for a knocked-out ally: the chooser's HP call reads 0, and the command test
// reads "no". The greyed portrait (player_two/party_switch.h) then means what it shows.
//
// The command test is gated at its own first instruction (player_two/command_rules_code.h), which
// covers all five of its callers. The member's type and menu ordinal are read once per engine tick
// (Update), with guarded calls, not inside the menu's own tick.
//
// Also logged each frame while player 2 plays, on change: the command controller's members,
// battle-HUD and menu bytes, its menu requests, and the world's battle-command speed slot (the
// menu's time slow), for the next stages (player 2's own menu). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_rules {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick: player 2's member's type and command-menu ordinal read, or cleared when player
/// 2 has none.
void Update() noexcept;

/// Once per frame while player 2 plays: the command controller logged on change.
void Watch() noexcept;

}  // namespace lss::player_two::command_rules
