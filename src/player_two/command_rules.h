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
// One of the command test's five calls (0x1428F450E) straddles a code block and is not redirected;
// the chooser's HP lever still keeps that member out of ally mode's switching.
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

/// Once per frame while player 2 plays: the command controller logged on change.
void Watch() noexcept;

}  // namespace lss::player_two::command_rules
