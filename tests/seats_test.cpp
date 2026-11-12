// Host-side tests for the seats header (players/seats.h): the seat numbering, the names and the bounds the session
// and the per-seat records index by. The header carries its own player count (there is no scopes header).

#include "players/seats.h"

#include "expect.h"

#include <cstdio>
#include <cwchar>

namespace {

using namespace lss::players;

bool Same(const wchar_t* a, const wchar_t* b)
{
    return std::wcscmp(a, b) == 0;
}

void TestNumbering()
{
    std::puts("seats: player 1 is no seat; the built seats fit the records");
    static_assert(kSeatsBuilt >= 1, "at least player 2");
    EXPECT(!SeatBuilt(0));  // player 1 is the game's, never a seat
    EXPECT(SeatBuilt(1));  // player 2 is seat 1
    EXPECT(!SeatBuilt(kSeatsBuilt + 1));
    EXPECT(!SeatBuilt(-1));
    EXPECT(!SeatIndex(0));
    EXPECT(SeatIndex(1) && SeatIndex(2));
    EXPECT(!SeatIndex(kMaxPlayers));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        EXPECT(SeatIndex(seat));  // every built seat has its records
    }
}

void TestNames()
{
    std::puts("seats: each seat's name");
    EXPECT(Same(SeatName(0), L"player 1"));
    EXPECT(Same(SeatName(1), L"player 2"));
    EXPECT(Same(SeatName(2), L"player 3"));
    EXPECT(Same(SeatName(-1), L"a player"));
    EXPECT(Same(SeatName(kMaxPlayers), L"a player"));
    static_assert(SeatName(2)[7] == L'3', "constexpr name");
}

}  // namespace

int main()
{
    TestNumbering();
    TestNames();
    return lss::test::Report();
}
