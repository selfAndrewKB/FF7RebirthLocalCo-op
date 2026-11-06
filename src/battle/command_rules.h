#pragma once

#include <cstdint>

// Who may issue commands to whom in the battle command menu (RE-p2-command-menu.md §4, pass S-CMD1).
//
// The leader commands an ally through the command menu's ally mode (LT/RT, the shortcut palette):
// the menu's chooser walks the members and skips one with no HP (knocked out) or petrified, and a
// second test (the member can take a command: has usable commands, not stopped, asleep, petrified or
// blocked) guards the strict path, the ally pick and the ally-mode portraits. The portrait greying
// alone blocks nothing. Player 1 must not command the member a further player plays, so both levers answer
// for that member as for a knocked-out ally: the chooser's HP call reads 0, and the command test
// reads "no". The greyed portrait (session/party_switch.h) then means what it shows.
//
// The command test is gated at its own first instruction (battle/command_rules_code.h), which
// covers all five of its callers. The member's type and menu ordinal are read once per engine tick
// (Update), with guarded calls, not inside the menu's own tick. Game thread only.
//
// Per seat: the refused members are a set, as bit masks (ordinals for the command test, member types for the HP lever:
// ally mode and the menu agree). Player 1 refuses every playing seat's member; a seat's pass (SeatRules) refuses player
// 1's leader and every other seat's member. The gate is a bit test (command_rules_code.h, CommandGateMaskCode): the
// five callers pass an ordinal from 0 to the party count less one (at most 16), or -1 for none, which reads bit 31,
// never set (RE-seats-step6-battle.md §2).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_rules {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick: every playing seat's member's type and command-menu ordinal read (player 1's refusals), or
/// cleared when no seat plays one.
void Update() noexcept;

/// While alive (`seat`'s command controller ticks): the rules turned around for that seat, so the members that may not
/// be commanded are player 1's leader (type `leaderType`) and every other seat's member.
class SeatRules
{
public:
    SeatRules(int seat, std::uint8_t leaderType) noexcept;
    ~SeatRules();
    SeatRules(const SeatRules&)            = delete;
    SeatRules& operator=(const SeatRules&) = delete;

private:
    std::uint32_t savedOrdinals_ = 0, savedTypes_ = 0;
};

}  // namespace lss::battle::command_rules
