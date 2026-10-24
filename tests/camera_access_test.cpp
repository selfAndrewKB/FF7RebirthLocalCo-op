// Host-side tests for the pure seat lookup in player_two/camera/camera_access.h: the seat whose camera manager (or
// controller) an object the engine hands a redirect is, from the per-seat owners as the controllers name them.

#include "player_two/camera/camera_access.h"

#include "expect.h"

#include <cstdio>

namespace {

using lss::player_two::camera_access::SeatIn;
namespace players = lss::players;

int g_objects[4];  // stand-ins for camera managers

void TestEachSeatFound()
{
    std::puts("camera access: each seat's camera manager names its seat, player 1's seat 0");
    const void* owners[players::kMaxPlayers] = {&g_objects[0], &g_objects[1], &g_objects[2]};
    EXPECT(SeatIn(owners, &g_objects[0]) == 0);
    EXPECT(SeatIn(owners, &g_objects[1]) == players::kPlayerTwoSeat);
    EXPECT(SeatIn(owners, &g_objects[2]) == 2);
    EXPECT(SeatIn(owners, &g_objects[3]) == -1);  // nobody's
}

void TestNullNeverMatches()
{
    std::puts("camera access: an empty seat (no controller or inactive) and a null object match nothing");
    const void* owners[players::kMaxPlayers] = {&g_objects[0], nullptr, nullptr};
    EXPECT(SeatIn(owners, nullptr) == -1);
    EXPECT(SeatIn(owners, &g_objects[1]) == -1);
    static_assert(SeatIn({nullptr, nullptr, nullptr}, nullptr) == -1, "constexpr, null never matches");
}

void TestFirstSeatWinsOnAShare()
{
    std::puts("camera access: two seats naming one object (a leave half done) give the lower seat");
    const void* owners[players::kMaxPlayers] = {&g_objects[0], &g_objects[1], &g_objects[1]};
    EXPECT(SeatIn(owners, &g_objects[1]) == 1);
}

}  // namespace

int main()
{
    TestEachSeatFound();
    TestNullNeverMatches();
    TestFirstSeatWinsOnAShare();
    return lss::test::Report();
}
