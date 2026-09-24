// Host-side tests for views::TurnIfPlayer and NormalizeDegrees, run without the game.
//
// The view info is a zeroed 0x6A0-byte block (FMinimalViewInfo's size in this build) with a yaw
// written at +0x10. Only the turned player's yaw may change, by exactly the turn, wrapped into
// (-180, 180]; every other byte stays as it was.

#include "views/view_turn.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using lss::views::NormalizeDegrees;
using lss::views::TurnIfPlayer;
namespace view_info = lss::views::view_info;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

struct ViewInfo
{
    std::array<std::byte, 0x6A0> bytes{};

    explicit ViewInfo(float yaw) { std::memcpy(bytes.data() + view_info::kYaw, &yaw, sizeof(yaw)); }
    float Yaw() const
    {
        float yaw = 0.0f;
        std::memcpy(&yaw, bytes.data() + view_info::kYaw, sizeof(yaw));
        return yaw;
    }
};

void TestNormalize()
{
    std::puts("normalize: into (-180, 180]");
    EXPECT(Near(NormalizeDegrees(0.0f), 0.0f));
    EXPECT(Near(NormalizeDegrees(180.0f), 180.0f));
    EXPECT(Near(NormalizeDegrees(-180.0f), 180.0f));
    EXPECT(Near(NormalizeDegrees(200.0f), -160.0f));
    EXPECT(Near(NormalizeDegrees(-200.0f), 160.0f));
    EXPECT(Near(NormalizeDegrees(725.0f), 5.0f));
}

void TestOnlyTheTurnedPlayer()
{
    std::puts("turn: only the turned player, by exactly the turn, nothing else changed");
    int playerOne = 0;
    int playerTwo = 0;

    ViewInfo one(10.0f);
    const auto before = one.bytes;
    EXPECT(!TurnIfPlayer(&playerOne, &playerTwo, 30.0f, one.bytes.data()));
    EXPECT(one.bytes == before);

    ViewInfo two(10.0f);
    auto expected = two.bytes;
    EXPECT(TurnIfPlayer(&playerTwo, &playerTwo, 30.0f, two.bytes.data()));
    EXPECT(Near(two.Yaw(), 40.0f));
    const float turned = 40.0f;
    std::memcpy(expected.data() + view_info::kYaw, &turned, sizeof(turned));
    EXPECT(two.bytes == expected);  // only the yaw moved

    ViewInfo wrap(170.0f);
    TurnIfPlayer(&playerTwo, &playerTwo, 30.0f, wrap.bytes.data());
    EXPECT(Near(wrap.Yaw(), -160.0f));
}

void TestNoPlayerTwo()
{
    std::puts("turn: nothing when there is no player 2");
    int playerOne = 0;
    ViewInfo view(10.0f);
    EXPECT(!TurnIfPlayer(&playerOne, nullptr, 30.0f, view.bytes.data()));
    EXPECT(!TurnIfPlayer(nullptr, nullptr, 30.0f, view.bytes.data()));
    EXPECT(Near(view.Yaw(), 10.0f));
}

}  // namespace

int main()
{
    TestNormalize();
    TestOnlyTheTurnedPlayer();
    TestNoPlayerTwo();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
