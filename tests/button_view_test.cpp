// Host-side tests for player_two::button_view, run without the game.
//
// The source bits are the ones seen live: pad 1 = 0x1, pad 2 = 0x2, keyboard slot 0 = 0x20000.

#include "player_two/button_view.h"

#include <cstdio>

namespace {

using lss::player_two::button_view::Held;
using lss::player_two::button_view::PadBits;
using lss::player_two::button_view::Views;

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

constexpr std::uint32_t kPadOne   = 0x1;
constexpr std::uint32_t kPadTwo   = 0x2;
constexpr std::uint32_t kKeyboard = 0x20000;

bool Pressed(const Views& views, int player)
{
    return !views.previous[player] && views.now[player];
}

void TestSourceBits()
{
    std::puts("buttons: pad bits are XInput min(id,3) and DirectInput 21+id");
    EXPECT(PadBits(0) == (0x1u | (1u << 21)));
    EXPECT(PadBits(1) == (0x2u | (1u << 22)));
    EXPECT(PadBits(5) == ((1u << 3) | (1u << 26)));
}

void TestEachPlayerSeesOnlyTheirSources()
{
    std::puts("buttons: pad 1 and the keyboard are player 1's, pad 2 is player 2's");
    EXPECT(Held(1, kPadOne, 0) && !Held(1, kPadOne, 1));
    EXPECT(!Held(1, kPadTwo, 0) && Held(1, kPadTwo, 1));
    EXPECT(Held(1, kKeyboard, 0) && !Held(1, kKeyboard, 1));
    EXPECT(Held(1, kPadOne | kPadTwo, 0) && Held(1, kPadOne | kPadTwo, 1));
}

void TestConsumedPressIsNotHeld()
{
    std::puts("buttons: a merged release that leaves the mask set is a release for both");
    EXPECT(!Held(0, kPadOne, 0) && !Held(0, kPadTwo, 1));
}

void TestPadTwoPressWhilePadOneHolds()
{
    std::puts("buttons: pad 2's press while pad 1 holds is player 2's edge, and no edge for player 1");
    Views views;
    views.Advance(1, kPadOne);  // pad 1 presses
    EXPECT(Pressed(views, 0) && !Pressed(views, 1));
    views.Advance(1, kPadOne);  // held
    EXPECT(!Pressed(views, 0) && !Pressed(views, 1));
    views.Advance(1, kPadOne | kPadTwo);  // pad 2 presses (the merged byte gives no edge)
    EXPECT(!Pressed(views, 0) && Pressed(views, 1));
    views.Advance(1, kPadTwo);  // pad 1 lets go; pad 2 still holds
    EXPECT(!views.now[0] && views.now[1] && !Pressed(views, 1));
    views.Advance(1, kPadOne | kPadTwo);  // pad 1 presses again: its own edge
    EXPECT(Pressed(views, 0) && !Pressed(views, 1));
}

/// One frame as the game leaves the byte: `byte` if the game wrote it, else what the mod left.
void Frame(Views& views, std::uint32_t sources, int gameWrote = -1)
{
    const std::uint8_t byte = gameWrote >= 0 ? static_cast<std::uint8_t>(gameWrote) : views.left;
    views.Advance(byte, sources);
}

void TestPadTwoHoldSurvivesPlayerOnesView()
{
    std::puts("buttons: pad 2's hold stays held for player 2 although player 1's 0 is left in the byte");
    Views views;
    Frame(views, kPadTwo, 1);  // pad 2 presses: the game writes 1
    EXPECT(Pressed(views, 1) && !views.now[0] && views.left == 0);
    for (int frame = 0; frame < 5; ++frame)
    {
        Frame(views, kPadTwo);  // pad 2 holds: the game leaves the mod's 0 alone
        EXPECT(views.now[1] && !Pressed(views, 1) && !views.now[0]);
    }
    Frame(views, 0, 0);  // pad 2 lets go: the game's release
    EXPECT(!views.now[1] && views.previous[1]);
}

void TestPadOneReleaseWhilePadTwoHolds()
{
    std::puts("buttons: pad 1 lets go while pad 2 holds: player 1 released, player 2 still held");
    Views views;
    Frame(views, kPadOne, 1);
    Frame(views, kPadOne | kPadTwo);  // pad 2 joins (no rewrite: held already)
    EXPECT(views.now[0] && Pressed(views, 1));
    Frame(views, kPadTwo);  // pad 1 lets go: the mask keeps pad 2, the game writes nothing
    EXPECT(!views.now[0] && views.now[1]);
    Frame(views, kPadTwo);  // the mod left 0 for player 1; player 2 still holds
    EXPECT(!views.now[0] && views.now[1]);
    Frame(views, kPadOne | kPadTwo, 1);  // pad 1 presses again: the game writes 1 over the mod's 0
    EXPECT(Pressed(views, 0) && views.now[1]);
}

void TestGameReleaseWithMaskSet()
{
    std::puts("buttons: the game dropping held with the mask still set is seen when it differs from the mod's byte");
    Views views;
    Frame(views, kPadOne, 1);
    Frame(views, kPadOne, 0);  // a game writer clears held (a consumed press)
    EXPECT(!views.now[0] && !views.now[1]);
}

}  // namespace

int main()
{
    TestSourceBits();
    TestEachPlayerSeesOnlyTheirSources();
    TestConsumedPressIsNotHeld();
    TestPadTwoPressWhilePadOneHolds();
    TestPadTwoHoldSurvivesPlayerOnesView();
    TestPadOneReleaseWhilePadTwoHolds();
    TestGameReleaseWithMaskSet();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
