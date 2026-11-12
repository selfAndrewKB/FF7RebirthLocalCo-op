#pragma once

// The view columns: how many views the screen shows and whose each one is. Pure rules and
// constants, no game; host-tested (tests/columns_test.cpp).
//
// A column is one view slot of the engine's local player list, laid out by the engine: two players side by side (or
// top and bottom, the player's choice), three as the engine's three equal columns (ThreePlayerSplitscreenLayout
// Vertical). Column 0 is player 1's: the list's first, which no leave removes. The seats in (players/seats.h) follow
// in the order they joined (a join appends; a leave closes the list up), and the mirror views fill the rest: a mirror
// is a local player of the mod's that only shows player 1's camera (views/mirror_player.h), so a column no player
// has is never black while more columns are held than players are in. A column is a list index; a seat is a
// controller id; the two are never taken for each other.
//
// The columns a frame shows are the players in, or more while the columns are held: the three-player monitor mode
// (the span held over a row of three monitors, views/window_span.h) or the console's `view-columns` (the checks on one
// monitor).
//
// A mirror's controller id is never a seat's: a view draws the HUD of the seat its controller id names
// (FSceneView::PlayerIndex, hud/passes/column_hud_images.h), so a mirror at a seat's id would draw that seat's HUD. The
// mirrors take ids past the seats' and past XInput's four users (players::kFirstMirrorId on: 4 and 5), so no pad
// ever drives them (the engine routes XInput user N to controller id N).

#include "players/seats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::views::columns {

/// The most columns the mod lays out: one per player (player 1 and two seats), the engine's three-column layout.
inline constexpr int kMaxColumns = players::kMaxPlayers;

/// Every column but player 1's may be a mirror's, each with its own controller id (players/seats.h, after the seats').
inline constexpr int kMirrorsBuilt = kMaxColumns - 1;
static_assert(kMirrorsBuilt == players::kMirrorIds, "one mirror id per column beside player 1's");
using players::IsMirrorId;
using players::kFirstMirrorId;

/// The columns a frame shows. `playersIn`: player 1 and the seats with a local player; `held`: the columns held
/// beyond the players (the monitor mode's, or the console's), 0 for none. Never fewer than the players, at least one,
/// at most kMaxColumns.
constexpr int Wanted(int playersIn, int held) noexcept
{
    return std::clamp(std::max(playersIn, held), 1, kMaxColumns);
}

/// What fills a column.
inline constexpr int kMirror  = -2;  // a mirror view
inline constexpr int kUnknown = -1;  // a local player the mod does not know (an unread id, a stranger)

/// The engine's local player list as columns: each column's seat (or kMirror, kUnknown) and each seat's column.
struct Map
{
    int                                count = 0;  // local players listed, one column each while the screen splits
    std::array<int, kMaxColumns>       seatAt{kUnknown, kUnknown, kUnknown};
    std::array<std::int32_t, kMaxColumns> idAt{-1, -1, -1};  // each column's controller id as listed (-1 unread)
    std::array<int, players::kMaxPlayers> columnOf{-1, -1, -1};  // -1: the seat is not listed
    int                                mirrors = 0;

    /// Player 1 and the seats listed.
    int PlayersListed() const noexcept
    {
        int n = 0;
        for (const int column : columnOf)
        {
            n += column >= 0 ? 1 : 0;
        }
        return n;
    }

    bool operator==(const Map&) const noexcept = default;
};

/// The map of a list whose controller ids are `ids`, in list order (-1 for one that could not be read). A list longer
/// than kMaxColumns counts only its first kMaxColumns (the engine lays out no more as columns); a seat listed twice
/// keeps its first column.
constexpr Map MapOf(std::span<const std::int32_t> ids) noexcept
{
    Map map;
    map.count = static_cast<int>(std::min<std::size_t>(ids.size(), kMaxColumns));
    for (int column = 0; column < map.count; ++column)
    {
        const std::int32_t id = ids[static_cast<std::size_t>(column)];
        map.idAt[static_cast<std::size_t>(column)] = id;
        if (IsMirrorId(id))
        {
            map.seatAt[static_cast<std::size_t>(column)] = kMirror;
            ++map.mirrors;
        }
        else if (id >= 0 && id < players::kMaxPlayers && map.columnOf[static_cast<std::size_t>(id)] < 0)
        {
            map.seatAt[static_cast<std::size_t>(column)]  = id;
            map.columnOf[static_cast<std::size_t>(id)] = column;
        }
    }
    return map;
}

}  // namespace lss::views::columns
