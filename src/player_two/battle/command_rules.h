#pragma once

#include <cstdint>

// Who may issue commands to whom in the battle command menu (RE-p2-command-menu.md §4, pass S-CMD1).
//
// The leader commands an ally through the command menu's ally mode (LT/RT, the shortcut palette):
// the menu's chooser walks the members and skips one with no HP (knocked out) or petrified, and a
// second test (the member can take a command: has usable commands, not stopped, asleep, petrified or
// blocked) guards the strict path, the ally pick and the ally-mode portraits. The portrait greying
// alone blocks nothing. Player 1 must not command the member player 2 plays, so both levers answer
// for that member as for a knocked-out ally: the chooser's HP call reads 0, and the command test
// reads "no". The greyed portrait (player_two/session/party_switch.h) then means what it shows.
//
// The command test is gated at its own first instruction (player_two/battle/command_rules_code.h), which
// covers all five of its callers. The member's type and menu ordinal are read once per engine tick
// (Update), with guarded calls, not inside the menu's own tick. Game thread only.
//
// Player 2's only (players::kPlayerTwoSeat): the gate's machine code compares one ordinal. A set (player 1 refusing
// every seat's member, a seat refusing player 1's leader and the other seats' members) fits the same bytes as a bit
// test (command_rules_code.h, CommandGateMaskCode, host-tested, not installed), which reads ordinals modulo 32: step 6
// installs it, with the HP lever widened alike, once IDA shows no caller passes an ordinal outside 0..31
// (static_assert in command_rules.cpp).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::command_rules {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick: player 2's member's type and command-menu ordinal read, or cleared when player
/// 2 has none.
void Update() noexcept;

/// While alive (player 2's command controller ticks): the rules turned around, so the member that may
/// not be commanded is player 1's leader (type `leaderType`), not player 2's member.
class PlayerTwoRules
{
public:
    explicit PlayerTwoRules(std::uint8_t leaderType) noexcept;
    ~PlayerTwoRules();
    PlayerTwoRules(const PlayerTwoRules&)            = delete;
    PlayerTwoRules& operator=(const PlayerTwoRules&) = delete;

private:
    std::int32_t savedGate_ = -1, savedType_ = -1;
};

}  // namespace lss::player_two::command_rules
