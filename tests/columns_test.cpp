// Host-side tests for the view columns (views/columns.h): how many columns a frame shows, the mirrors' controller ids,
// and the engine's list read as columns.

#include "views/columns.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using namespace lss::views::columns;

void TestWanted()
{
    std::puts("columns: the players in, or the columns held");
    EXPECT(Wanted(1, 0) == 1);  // player 1 alone, nothing held
    EXPECT(Wanted(2, 0) == 2);  // player 2 in: two columns
    EXPECT(Wanted(1, 3) == 3);  // the monitor mode with player 1 alone: two mirrors
    EXPECT(Wanted(2, 3) == 3);  // ... with player 2 in: one mirror
    EXPECT(Wanted(3, 3) == 3);
    EXPECT(Wanted(3, 0) == 3);  // never fewer than the players
    EXPECT(Wanted(2, 1) == 2);
    EXPECT(Wanted(0, 0) == 1);  // at least one
    EXPECT(Wanted(1, 9) == kMaxColumns);
    static_assert(Wanted(2, 0) == 2, "constexpr");
}

void TestMirrorIds()
{
    std::puts("columns: a mirror's controller id is never a seat's");
    static_assert(kMaxColumns == 3 && kMirrorsBuilt == 2, "two mirrors at most beside player 1");
    for (int seat = 0; seat < lss::players::kMaxPlayers; ++seat)
    {
        EXPECT(!IsMirrorId(seat));
    }
    EXPECT(IsMirrorId(kFirstMirrorId));
    EXPECT(IsMirrorId(kFirstMirrorId + kMirrorsBuilt - 1));
    EXPECT(!IsMirrorId(kFirstMirrorId + kMirrorsBuilt));
    EXPECT(!IsMirrorId(-1));
    for (int pad = 0; pad < lss::players::kXInputUsers; ++pad)
    {
        EXPECT(!IsMirrorId(pad));  // no pad reaches a mirror's controller
    }
}

void TestMap()
{
    std::puts("columns: the list read as columns");
    {
        const std::array<std::int32_t, 1> ids{0};
        const Map map = MapOf(ids);
        EXPECT(map.count == 1 && map.mirrors == 0 && map.PlayersListed() == 1);
        EXPECT(map.seatAt[0] == 0 && map.columnOf[0] == 0 && map.columnOf[1] == -1);
    }
    {
        // Player 2 in: column 1 is seat 1's.
        const std::array<std::int32_t, 2> ids{0, 1};
        const Map map = MapOf(ids);
        EXPECT(map.count == 2 && map.columnOf[1] == 1 && map.seatAt[1] == 1 && map.PlayersListed() == 2);
    }
    {
        // The monitor mode, player 1 alone: two mirrors.
        const std::array<std::int32_t, 3> ids{0, kFirstMirrorId, kFirstMirrorId + 1};
        const Map map = MapOf(ids);
        EXPECT(map.count == 3 && map.mirrors == 2 && map.PlayersListed() == 1);
        EXPECT(map.seatAt[1] == kMirror && map.seatAt[2] == kMirror);
    }
    {
        // Player 3 in with player 2 out: seat 2 in column 1, the mirror after it.
        const std::array<std::int32_t, 3> ids{0, 2, kFirstMirrorId};
        const Map map = MapOf(ids);
        EXPECT(map.columnOf[2] == 1 && map.columnOf[1] == -1 && map.seatAt[2] == kMirror && map.mirrors == 1);
    }
    {
        // An unread id and a stranger are no one's; a seat listed twice keeps its first column.
        const std::array<std::int32_t, 3> ids{0, -1, 0};
        const Map map = MapOf(ids);
        EXPECT(map.seatAt[1] == kUnknown && map.seatAt[2] == kUnknown && map.columnOf[0] == 0);
        EXPECT(map.PlayersListed() == 1 && map.mirrors == 0);
    }
    {
        // More listed than columns: the first kMaxColumns only.
        const std::array<std::int32_t, 4> ids{0, 1, kFirstMirrorId, kFirstMirrorId + 1};
        const Map map = MapOf(ids);
        EXPECT(map.count == kMaxColumns && map.mirrors == 1);
    }
    {
        const Map map = MapOf({});
        EXPECT(map.count == 0 && map.PlayersListed() == 0);
    }
}

}  // namespace

int main()
{
    TestWanted();
    TestMirrorIds();
    TestMap();
    return lss::test::Report();
}
