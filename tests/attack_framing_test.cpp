// Host-side tests for the pure pieces of camera/attack_framing.h: the per-camera latch key (the seat in the
// key) and the rule for which cameras a shot of a player's character stays on, checked against the two-player rule
// (world_rules::ShotShows) it replaces.

#include "camera/attack_framing.h"
#include "world/world_rules.h"

#include "expect.h"

#include <cstdint>
#include <cstdio>

namespace {

using lss::camera::attack_framing::LatchKey;
using lss::camera::attack_framing::ShotShowsOn;
namespace players = lss::players;

int g_chars[6];  // stand-ins: player 1, player 2's member, player 3's member, an ally, an enemy, nobody

void TestLatchKeyKeepsTwoPlayerValue()
{
    std::puts("attack framing: player 1's key is the weak value, player 2's has bit 63 flipped (as before)");
    constexpr std::uint64_t key = 0x0000123400000056ull;
    static_assert(LatchKey(key, 0) == key, "player 1's camera: the weak value");
    EXPECT(LatchKey(key, players::kPlayerTwoSeat) == (key ^ 0x8000000000000000ull));
}

void TestLatchKeysApart()
{
    std::puts("attack framing: the three cameras' keys for one sequence are distinct");
    for (const std::uint64_t key : {0x0ull, 0x0000123400000056ull, 0xC000000000000001ull})
    {
        EXPECT(LatchKey(key, 0) != LatchKey(key, 1));
        EXPECT(LatchKey(key, 0) != LatchKey(key, 2));
        EXPECT(LatchKey(key, 1) != LatchKey(key, 2));
    }
    EXPECT(LatchKey(7, -1) == 7 && LatchKey(7, players::kMaxPlayers) == 7);  // no seat: the weak value
}

void TestTwoPlayerRuleUnchanged()
{
    std::puts("attack framing: with two players the rule answers as world_rules::ShotShows did, player 1's character "
              "present or null (every non-null target: a null one is never decided)");
    const void* member = &g_chars[1];
    for (const void* one : {static_cast<const void*>(&g_chars[0]), static_cast<const void*>(nullptr)})
    {
        const void* humans[players::kMaxPlayers] = {one, member, nullptr};
        const void* targets[] = {one, member, &g_chars[3], &g_chars[4]};
        const void* owners[]  = {&g_chars[0], member, &g_chars[3], &g_chars[4], nullptr};
        for (const void* target : targets)
        {
            if (target == nullptr)
            {
                continue;  // Decide returns undecided for a null target before the rule
            }
            for (const void* owner : owners)
            {
                for (int seat = 0; seat <= 1; ++seat)
                {
                    EXPECT(ShotShowsOn(target, humans, seat, owner) ==
                           lss::world::world_rules::ShotShows(target, member, one, seat == 1, owner));
                }
            }
        }
    }
}

void TestThreePlayers()
{
    std::puts("attack framing: with three players a player's target shows on its own camera, and on another only for "
              "that camera's own shot");
    const void* one    = &g_chars[0];
    const void* two    = &g_chars[1];
    const void* three  = &g_chars[2];
    const void* enemy  = &g_chars[4];
    const void* humans[players::kMaxPlayers] = {one, two, three};
    EXPECT(ShotShowsOn(three, humans, 2, enemy));    // player 3 grabbed: on player 3's camera
    EXPECT(!ShotShowsOn(three, humans, 0, enemy));   // not on player 1's
    EXPECT(!ShotShowsOn(three, humans, 1, enemy));   // nor player 2's
    EXPECT(ShotShowsOn(three, humans, 1, two));      // player 2's own move on player 3: on player 2's camera
    EXPECT(ShotShowsOn(enemy, humans, 1, three));    // an enemy targeted: every camera
    EXPECT(ShotShowsOn(&g_chars[3], humans, 2, enemy));  // the AI ally targeted: every camera
    const void* twoOnly[players::kMaxPlayers] = {one, two, nullptr};
    EXPECT(ShotShowsOn(three, twoOnly, 1, enemy));   // player 3 not in: its character is nobody's here
    EXPECT(!ShotShowsOn(one, twoOnly, 2, enemy));    // a camera with no player owns nothing
}

}  // namespace

int main()
{
    TestLatchKeyKeepsTwoPlayerValue();
    TestLatchKeysApart();
    TestTwoPlayerRuleUnchanged();
    TestThreePlayers();
    return lss::test::Report();
}
