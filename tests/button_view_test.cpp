// Host-side tests for input::button_view, run without the game.
//
// The source bits are the ones seen live: pad 1 = 0x1, pad 2 = 0x2, keyboard slot 0 = 0x20000.

#include "input/button_view.h"

#include "expect.h"

#include <cstdio>

namespace {

using lss::input::button_view::Held;
using lss::input::button_view::kKeyboardBits;
using lss::input::button_view::PlayerSources;
using lss::input::button_view::SeatBit;
using lss::input::button_view::SeatOfSource;
using lss::input::button_view::SeatPads;
using lss::input::button_view::SeatSources;
using lss::input::button_view::PadBits;
using lss::input::button_view::Views;

constexpr std::uint32_t kPadOne   = 0x1;
constexpr std::uint32_t kPadTwo   = 0x2;
constexpr std::uint32_t kKeyboard = 0x20000;
constexpr std::uint32_t kPadThree = 0x4;
constexpr unsigned      kTwoIn    = SeatBit(1);                // player 2 in: the two-player game
constexpr unsigned      kBothIn   = SeatBit(1) | SeatBit(2);  // players 2 and 3 in

bool Pressed(const Views& views, int player)
{
    return !views.previous[player] && views.now[player];
}

void TestSourceBits()
{
    std::puts("buttons: pad bits are XInput min(id,3), PlayStation 4+min(id,3) and DirectInput 21+id");
    EXPECT(PadBits(0) == (0x1u | (1u << 4) | (1u << 21)));
    EXPECT(PadBits(1) == (0x2u | (1u << 5) | (1u << 22)));
    EXPECT(PadBits(5) == ((1u << 3) | (1u << 7) | (1u << 26)));
}

void TestEachPlayerSeesOnlyTheirSources()
{
    std::puts("buttons: pad 1 and the keyboard are player 1's, pad 2 is player 2's");
    EXPECT(Held(1, kPadOne, 0, kTwoIn) && !Held(1, kPadOne, 1, kTwoIn));
    EXPECT(!Held(1, kPadTwo, 0, kTwoIn) && Held(1, kPadTwo, 1, kTwoIn));
    EXPECT(Held(1, kKeyboard, 0, kTwoIn) && !Held(1, kKeyboard, 1, kTwoIn));
    EXPECT(Held(1, kPadOne | kPadTwo, 0, kTwoIn) && Held(1, kPadOne | kPadTwo, 1, kTwoIn));
}

void TestConsumedPressIsNotHeld()
{
    std::puts("buttons: a merged release that leaves the mask set is a release for both");
    EXPECT(!Held(0, kPadOne, 0, kTwoIn) && !Held(0, kPadTwo, 1, kTwoIn));
}

void TestPadTwoPressWhilePadOneHolds()
{
    std::puts("buttons: pad 2's press while pad 1 holds is player 2's edge, and no edge for player 1");
    Views views;
    views.Advance(1, kPadOne, kTwoIn);  // pad 1 presses
    EXPECT(Pressed(views, 0) && !Pressed(views, 1));
    views.Advance(1, kPadOne, kTwoIn);  // held
    EXPECT(!Pressed(views, 0) && !Pressed(views, 1));
    views.Advance(1, kPadOne | kPadTwo, kTwoIn);  // pad 2 presses (the merged byte gives no edge)
    EXPECT(!Pressed(views, 0) && Pressed(views, 1));
    views.Advance(1, kPadTwo, kTwoIn);  // pad 1 lets go; pad 2 still holds
    EXPECT(!views.now[0] && views.now[1] && !Pressed(views, 1));
    views.Advance(1, kPadOne | kPadTwo, kTwoIn);  // pad 1 presses again: its own edge
    EXPECT(Pressed(views, 0) && !Pressed(views, 1));
}

/// One frame as the game leaves the byte: `byte` if the game wrote it, else what the mod left.
void Frame(Views& views, std::uint32_t sources, int gameWrote = -1)
{
    const std::uint8_t byte = gameWrote >= 0 ? static_cast<std::uint8_t>(gameWrote) : views.left;
    views.Advance(byte, sources, kTwoIn);
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

void TestTwoPlayerSourcesUnchanged()
{
    std::puts("buttons: with player 2 in, player 1 has everything but pad 2 (the two-player rule), pad 3 included");
    EXPECT(PlayerSources(0, kTwoIn) == ~PadBits(1));
    EXPECT(PlayerSources(1, kTwoIn) == PadBits(1));
    EXPECT(Held(1, kPadThree, 0, kTwoIn));  // pad 3 drives player 1 while player 3 is not in
    EXPECT(SeatOfSource(1, kTwoIn) == 1 && SeatOfSource(22, kTwoIn) == 1);  // XInput and DirectInput bit of pad 2
    EXPECT(SeatOfSource(5, kTwoIn) == 1 && SeatOfSource(4, kTwoIn) == 0);   // PlayStation pads on ids 1 and 0
    EXPECT(SeatOfSource(2, kTwoIn) == 0);  // pad 3's press is player 1's
    EXPECT(SeatOfSource(0, kTwoIn) == 0 && SeatOfSource(17, kTwoIn) == 0);  // pad 1, the keyboard
}

void TestKeyboardPlaysASeat()
{
    std::puts("buttons: while the keyboard plays player 3, its keys and the mouse are player 3's alone; once player 3 is "
              "out they are player 1's again");
    constexpr std::uint32_t kMouse = 1u << 19;
    EXPECT(SeatSources(2, 2) == (PadBits(2) | kKeyboardBits) && SeatSources(1, 2) == PadBits(1));
    EXPECT(Held(1, kKeyboard, 2, kBothIn, 2) && !Held(1, kKeyboard, 0, kBothIn, 2) && !Held(1, kKeyboard, 1, kBothIn, 2));
    EXPECT(Held(1, kMouse, 2, kBothIn, 2) && !Held(1, kMouse, 0, kBothIn, 2));
    EXPECT(SeatOfSource(17, kBothIn, 2) == 2 && SeatOfSource(18, kBothIn, 2) == 2 && SeatOfSource(19, kBothIn, 2) == 2);
    EXPECT(SeatOfSource(17, kTwoIn, 2) == 0 && Held(1, kKeyboard, 0, kTwoIn, 2));  // player 3 out: player 1's
    EXPECT(Held(1, kKeyboard, 0, kBothIn, 0) && Held(1, kKeyboard, 0, kBothIn, -1));  // beside player 1
    Views views;
    views.Advance(1, kKeyboard, kBothIn, 2);
    EXPECT(views.now[2] && !views.now[0] && !views.now[1]);
}
void TestThreePlayers()
{
    std::puts("buttons: with players 2 and 3 in, each seat sees its own pad and player 1 neither");
    EXPECT(SeatPads(kBothIn) == (PadBits(1) | PadBits(2)));
    EXPECT(SeatPads(kBothIn | SeatBit(0)) == SeatPads(kBothIn));  // player 1's bit is no pad of a seat
    EXPECT(PlayerSources(0, kBothIn) == ~(PadBits(1) | PadBits(2)));
    EXPECT(Held(1, kPadThree, 2, kBothIn) && !Held(1, kPadThree, 0, kBothIn) && !Held(1, kPadThree, 1, kBothIn));
    EXPECT(Held(1, kPadTwo, 1, kBothIn) && !Held(1, kPadTwo, 2, kBothIn));
    EXPECT(Held(1, kKeyboard, 0, kBothIn) && !Held(1, kKeyboard, 2, kBothIn));
    EXPECT(SeatOfSource(2, kBothIn) == 2 && SeatOfSource(23, kBothIn) == 2);
    EXPECT(SeatOfSource(2, SeatBit(2)) == 2 && SeatOfSource(1, SeatBit(2)) == 0);  // player 3 in alone
    static_assert(SeatOfSource(1, SeatBit(1)) == 1, "constexpr");
}

void TestMergedViewWritten()
{
    std::puts("buttons: the game's merged state written back (a menu opener) keeps the game's own edges for any pad");
    Views views;
    Frame(views, kPadTwo, 1);  // pad 2 presses Start: the game writes 1
    EXPECT(views.gamePrevious == 0 && views.gameHeld == 1);  // the game's own "pressed" for the pause poll
    views.Wrote(views.gameHeld);                             // the mod wrote the merged state, not player 1's 0
    EXPECT(views.left == 1);
    Frame(views, kPadTwo);  // held: the game leaves the 1 the mod wrote (equal: not a game write)
    EXPECT(views.gamePrevious == 1 && views.gameHeld == 1 && views.now[1] && !Pressed(views, 1));
    views.Wrote(views.gameHeld);
    Frame(views, kPadTwo, 0);  // a game writer clears held with the mask set (a consumed press): seen
    EXPECT(views.gameHeld == 0 && views.gamePrevious == 1 && !views.now[1]);
    views.Wrote(views.gameHeld);
    Frame(views, 0, 0);  // the release
    EXPECT(views.gameHeld == 0 && views.gamePrevious == 0);
    Frame(views, kPadOne | kPadTwo, 1);  // both press at once: one game edge, both players' edges
    EXPECT(views.gamePrevious == 0 && views.gameHeld == 1 && Pressed(views, 0) && Pressed(views, 1));
    EXPECT(views.left == 1);  // player 1's view and the merged state agree: no Wrote needed
}

void TestMergedViewFlipsMidHold()
{
    std::puts("buttons: a hold that spans a menu opening and closing (merged state in, player 1's view out) has no false edge");
    Views views;
    Frame(views, kPadTwo, 1);  // pad 2 presses in the field: player 1's 0 is left in the byte
    EXPECT(Pressed(views, 1) && views.left == 0);
    Frame(views, kPadTwo);  // held in the field
    EXPECT(views.now[1] && !Pressed(views, 1) && views.gameHeld == 1);
    views.Wrote(1);  // a menu opens: the merged state (1) is written over the 0
    Frame(views, kPadTwo);  // held under the menu: the byte reads the 1 the mod wrote, not a game write
    EXPECT(views.gameHeld == 1 && views.gamePrevious == 1 && views.now[1] && !Pressed(views, 1) && !views.now[0]);
    views.Wrote(1);
    Frame(views, kPadTwo);  // the menu closes: player 1's view (0) is written again (left = 0 by Advance)
    EXPECT(views.gameHeld == 1 && views.left == 0 && views.now[1] && !Pressed(views, 1));
    Frame(views, kPadTwo);  // held in the field: the byte reads the mod's 0, no game write
    EXPECT(views.gameHeld == 1 && views.now[1] && !Pressed(views, 1) && !views.previous[0] && !views.now[0]);
    Frame(views, 0, 0);  // the release, seen once
    EXPECT(!views.now[1] && views.previous[1] && views.gameHeld == 0);
    Frame(views, 0, 0);
    EXPECT(!views.now[1] && !views.previous[1]);
}

void TestShopSeatViewWritten()
{
    std::puts("buttons: pad 1's hold while a seat's view is written (its shop) stays held for player 1");
    Views views;
    Frame(views, kPadOne, 1);  // pad 1 presses: the game writes 1
    EXPECT(Pressed(views, 0) && views.left == 1);
    views.Wrote(0);  // the shop seat's view (0) is written instead of player 1's
    Frame(views, kPadOne);  // the byte reads the 0 the mod wrote: no game write, the hold stays
    EXPECT(views.gameHeld == 1 && views.now[0] && !Pressed(views, 0) && !views.now[1]);
    views.Wrote(0);
    Frame(views, kPadOne, 0);  // a game writer clears held (a consumed press): the same 0 as the mod's, unseen
    EXPECT(views.gameHeld == 1 && views.now[0]);  // the one case the view cannot see (documented), until the release
    Frame(views, 0, 0);
    EXPECT(!views.now[0] && views.gameHeld == 0);
}

void TestThreePadsPressApart()
{
    std::puts("buttons: pad 3's press while pads 1 and 2 hold is player 3's edge only");
    Views views;
    views.Advance(1, kPadOne | kPadTwo, kBothIn);
    EXPECT(Pressed(views, 0) && Pressed(views, 1) && !Pressed(views, 2));
    views.Advance(1, kPadOne | kPadTwo | kPadThree, kBothIn);
    EXPECT(!Pressed(views, 0) && !Pressed(views, 1) && Pressed(views, 2));
    views.Advance(1, kPadThree, kBothIn);  // pads 1 and 2 let go
    EXPECT(!views.now[0] && !views.now[1] && views.now[2] && views.left == 0);
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
    TestTwoPlayerSourcesUnchanged();
    TestKeyboardPlaysASeat();
    TestThreePlayers();
    TestThreePadsPressApart();
    TestMergedViewWritten();
    TestMergedViewFlipsMidHold();
    TestShopSeatViewWritten();
    return lss::test::Report();
}
