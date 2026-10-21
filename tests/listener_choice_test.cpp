// Host-side tests for player_two::listener_choice, run without the game.
//
// The nearer-listener choice with its hysteresis, the quaternion rotation the device's push uses, the clip, the LOD
// and the missed-range rules.

#include "player_two/listener_choice.h"

#include "expect.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace lss::player_two::listener_choice;

constexpr float kHandOver = 0.8f;

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

void TestChooseNearer()
{
    std::puts("listener: first sight picks the nearer (a tie goes to player 1); then kept until the other is 20 % nearer");
    EXPECT(!ChooseNearer(false, false, 100, 200, kHandOver));
    EXPECT(ChooseNearer(false, false, 200, 100, kHandOver));
    EXPECT(!ChooseNearer(false, true, 100, 100, kHandOver));  // a tie: player 1, whatever was remembered

    // Player 2's: kept until player 1's is nearer than 0.8 x player 2's.
    EXPECT(ChooseNearer(true, true, 100, 100, kHandOver));
    EXPECT(ChooseNearer(true, true, 81, 100, kHandOver));
    EXPECT(!ChooseNearer(true, true, 79, 100, kHandOver));
    // Player 1's: kept until player 2's is nearer than 0.8 x player 1's.
    EXPECT(!ChooseNearer(true, false, 100, 100, kHandOver));
    EXPECT(!ChooseNearer(true, false, 100, 81, kHandOver));
    EXPECT(ChooseNearer(true, false, 100, 79, kHandOver));
    EXPECT(!ChooseNearer(true, false, 100, 80, kHandOver));  // not strictly nearer: no flip
}

void TestRotateVector()
{
    std::puts("rotate: the identity keeps a vector; a 90 degree yaw turns x into y and keeps z");
    const float identity[4] = {0, 0, 0, 1};
    auto        v           = RotateVector(identity, {1, 2, 3});
    EXPECT(Near(v[0], 1) && Near(v[1], 2) && Near(v[2], 3));

    const float s        = std::sqrt(0.5f);
    const float yaw90[4] = {0, 0, s, s};  // about z by 90 degrees
    v                    = RotateVector(yaw90, {1, 0, 0});
    EXPECT(Near(v[0], 0) && Near(v[1], 1) && Near(v[2], 0));
    v = RotateVector(yaw90, {0, 0, 1});
    EXPECT(Near(v[0], 0) && Near(v[1], 0) && Near(v[2], 1));
    v = RotateVector(yaw90, {0, 1, 0});
    EXPECT(Near(v[0], -1) && Near(v[1], 0) && Near(v[2], 0));
}

void TestClipLodAndRange()
{
    std::puts("clip: player 2's listener when nearer; LOD: kept at the finest, lowered only by a lower level; the range");
    EXPECT(NearerForClip(10, 20));
    EXPECT(!NearerForClip(20, 10));
    EXPECT(!NearerForClip(10, 10));
    EXPECT(!NearerForClip(std::nanf(""), 10));

    EXPECT(LodKeepsFirst(0));
    EXPECT(LodKeepsFirst(-1));
    EXPECT(!LodKeepsFirst(2));
    EXPECT(LodLowered(2, 1));
    EXPECT(LodLowered(2, 0));
    EXPECT(!LodLowered(2, 2));
    EXPECT(!LodLowered(2, 3));

    EXPECT(MissedByTwo(100, 1, 50));
    EXPECT(MissedByTwo(100, 2, 150));
    EXPECT(!MissedByTwo(100, 1, 100));
    EXPECT(!MissedByTwo(0, 1, 0));
    EXPECT(!MissedByTwo(-5, 1, 0));
    EXPECT(!MissedByTwo(std::nanf(""), 1, 0));
}

}  // namespace

int main()
{
    TestChooseNearer();
    TestRotateVector();
    TestClipLodAndRange();

    return lss::test::Report();
}
