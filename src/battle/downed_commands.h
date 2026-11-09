#pragma once

// A downed player still commands the AI allies (Rebirth/Documentation/research/RE-downed-ally-commands.md).
//
// In co-op every player stays on a knocked-out or petrified character until it is revived or cured
// (session/party_switch.h). Single player never keeps such a character: it switches away at once, so the command menu
// refuses every open, an ally's included, while the menu's own member is dead or petrified, and a per-tick sweep closes
// every member's menu then. A stopped or sleeping leader, which the game does keep, still opens the allies' menus
// through the shoulder buttons and the shortcut palette; its own rows are greyed.
//
// Here the same holds for a downed or petrified player: at the menu's two gates (the open, state 0's next state; the
// sweep) the game's own dead and petrify tests, when the menu's own member fails them, are asked about the member the
// menu is opening for instead (the ally, or the palette's target), as long as that is another member of the menu and
// not one the command rules refuse (battle/command_rules.h). The downed character's own menu is still refused, an ally
// that is down too is refused, and a standing player's answers stay the game's. In co-op only; players 2 and 3 get it
// in their own menu passes, which run the same two gates on their own controller.
//
// The four calls are redirected at start-up, before any world exists (three of them cross a code block on one build).
// Game thread only; elsewhere every call passes through.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::downed_commands {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::downed_commands
