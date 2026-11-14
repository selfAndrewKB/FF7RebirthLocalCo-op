#pragma once

// The low-health red edge's rules for each player's view (hud/low_health.h). Pure, no game; host-tested
// (tests/low_health_rules_test.cpp).
//
// Whose danger a controller's decision counts: a seat's controller its own member only; player 1's controller player
// 1's member and every member no seat plays (an AI member low lights player 1's view, as in single player). Whose view
// a view is: by its controller id, a seat's that seat's, player 1's or a mirror's (a mirror shows player 1's picture)
// player 1's. What a view takes: its player's own fade, never above the game's shared value, which carries every gate
// of the game's (a menu's fade-out, its return). A seat holds the shared filter by a count of its own, never by the
// game's one latch per filter type, which a second owner would clear under the first.

#include "players/seats.h"

#include <array>
#include <cstdint>

namespace lss::hud::low_health_rules {

inline constexpr int          kPlayers = players::kMaxPlayers;
inline constexpr std::uint8_t kNoType  = 0xFF;  // a seat that plays no member

/// The member type each player plays, by player (index 0 unused: player 1 counts every type no seat plays).
using Held = std::array<std::uint8_t, kPlayers>;

/// Whether member `type`'s HP counts for the danger of `player`'s controller (0 player 1's).
constexpr bool Counts(int player, std::uint8_t type, const Held& held) noexcept
{
    if (player > 0)
    {
        return player < kPlayers && held[static_cast<std::size_t>(player)] != kNoType &&
               held[static_cast<std::size_t>(player)] == type;
    }
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if (held[static_cast<std::size_t>(seat)] == type)
        {
            return false;
        }
    }
    return true;
}

/// The fade's target for a player: on while their controller shows a filter (Dying, or Stone, which every controller
/// decides alike for the whole party) and the edge is allowed (an event hides it through player 1's controller only),
/// else 0.
constexpr float Target(std::int32_t danger, bool allowed, float on) noexcept
{
    return danger != 0 && allowed ? on : 0.0f;
}

/// The intensity a player's view takes: their own fade, capped by the game's.
constexpr float ViewIntensity(float own, float game) noexcept
{
    return own < game ? own : game;
}

/// The player whose view has controller id `id`: a seat's id that seat (`seatIds`, by seat; index 0 unused), player
/// 1's id or a mirror's player 1, anything else (a scene capture's -1) -1.
constexpr int ViewPlayer(std::int32_t id, std::int32_t playerOneId, const std::array<std::int32_t, kPlayers>& seatIds) noexcept
{
    if (id < 0)
    {
        return -1;
    }
    if (id == playerOneId || players::IsMirrorId(id))
    {
        return 0;
    }
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if (seatIds[static_cast<std::size_t>(seat)] == id)
        {
            return seat;
        }
    }
    return -1;
}

/// A seat's holds of the shared filter, as bits.
inline constexpr std::uint8_t kHoldDying = 1;
inline constexpr std::uint8_t kHoldStone = 2;

/// What a seat's filter call does to its holds: the holds after it, the counts to take and the counts to give back.
struct HoldStep
{
    std::uint8_t holds = 0;
    std::uint8_t take  = 0;
    std::uint8_t give  = 0;
};

/// The holds a controller's danger value stands for: Dying's (1) the Dying count, Stone's (4) the Stone count. A hold
/// beyond them is given back: the game's own reset of a controller zeroes its value through the latch, not the count.
constexpr std::uint8_t HoldsFor(std::int32_t danger) noexcept
{
    return danger == 1 ? kHoldDying : danger == 4 ? kHoldStone : 0;
}

/// The decision turns `bit`'s filter on: a count taken unless the seat holds it already.
constexpr HoldStep OnEnable(std::uint8_t holds, std::uint8_t bit) noexcept
{
    return (holds & bit) != 0 ? HoldStep{holds, 0, 0} : HoldStep{static_cast<std::uint8_t>(holds | bit), bit, 0};
}

/// The decision turns a filter off: its count given back; the Dying filter's off gives back the Stone's too, as the
/// game's off of the Dying type clears the Stone type's latch.
constexpr HoldStep OnDisable(std::uint8_t holds, bool dying) noexcept
{
    const std::uint8_t off = dying ? static_cast<std::uint8_t>(kHoldDying | kHoldStone) : kHoldStone;
    return HoldStep{static_cast<std::uint8_t>(holds & ~off), 0, static_cast<std::uint8_t>(holds & off)};
}

}  // namespace lss::hud::low_health_rules
