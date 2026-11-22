#pragma once

// Who the game warps at a battle's start and end, as the setting decides it without the game (battle/battle_warps.h
// applies it at the game's two calls). Host-tested (tests/battle_warps_rules_test.cpp).

#include <cstdint>

namespace lss::battle::battle_warps_rules {

/// The setting `[Battle] battle-warps` (platform/settings.h kBattleWarps, the words `on` and `off` in this order).
enum class Mode : std::uint8_t
{
    Game,         // on: the game's warps for everyone
    PlayersStay,  // off: every player's own character stays; the others are warped
};

/// Whether the setting keeps this member where it is: `player` is who plays it (0 player 1, a seat, -1 none: a member no
/// player controls), `anchor` the player the battle's gather stands around (player 1's, 0, unless enemies noticed
/// another player; the gather never warps its anchor). Off keeps every player's own character but the anchor's.
constexpr bool Stays(Mode mode, int player, int anchor = 0) noexcept
{
    return mode == Mode::PlayersStay && player >= 0 && player != anchor;
}

}  // namespace lss::battle::battle_warps_rules
