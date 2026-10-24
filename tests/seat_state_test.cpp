// Host-side tests for a seat's state word (players/seat_state.h). Taken from Remake's tests/seat_state_test.cpp;
// changed with the header: "handed" is a seat in without its member, and InGame is new.

#include "players/seat_state.h"

#include "expect.h"

#include <cstdio>
#include <cwchar>

namespace {

using lss::players::InGame;
using lss::players::SeatFlags;
using lss::players::SeatState;
using lss::players::StateName;
using lss::players::StateOf;

bool Same(const wchar_t* a, const wchar_t* b)
{
    return std::wcscmp(a, b) == 0;
}

void TestOutOfTheGame()
{
    std::puts("seat_state: out, awaiting a member, or rejoining after a map load (the wait first)");
    EXPECT(StateOf({}) == SeatState::Out);
    EXPECT(StateOf({.awaiting = true}) == SeatState::Awaiting);
    EXPECT(StateOf({.rejoinPending = true}) == SeatState::Rejoining);
    EXPECT(StateOf({.awaiting = true, .rejoinPending = true}) == SeatState::Awaiting);
    // Stale member flags with the player out never make a word of their own.
    EXPECT(StateOf({.playsMember = true, .handed = true}) == SeatState::Out);
}

void TestInTheGame()
{
    std::puts("seat_state: in (between members), playing, or handed while the game has the member");
    EXPECT(StateOf({.in = true}) == SeatState::In);
    EXPECT(StateOf({.in = true, .playsMember = true}) == SeatState::Playing);
    EXPECT(StateOf({.in = true, .handed = true}) == SeatState::Handed);
    // Playing wins: a member driven is never handed.
    EXPECT(StateOf({.in = true, .playsMember = true, .handed = true}) == SeatState::Playing);
    // In the game, the out-of-game flags change nothing.
    EXPECT(StateOf({.in = true, .playsMember = true, .awaiting = true, .rejoinPending = true}) == SeatState::Playing);
    EXPECT(StateOf({.in = true, .awaiting = true}) == SeatState::In);
}

void TestInGame()
{
    std::puts("seat_state: in the game is in, playing or handed, as the flag `in` was");
    for (int bits = 0; bits < 32; ++bits)  // every combination of the five flags
    {
        const SeatFlags flags{.in            = (bits & 1) != 0,
                              .playsMember   = (bits & 2) != 0,
                              .handed        = (bits & 4) != 0,
                              .awaiting      = (bits & 8) != 0,
                              .rejoinPending = (bits & 16) != 0};
        EXPECT(InGame(StateOf(flags)) == flags.in);
        EXPECT((StateOf(flags) == SeatState::Playing) == (flags.in && flags.playsMember));
    }
}

void TestWords()
{
    std::puts("seat_state: the reading's words, as the console scripts wait for them");
    EXPECT(Same(StateName(SeatState::Out), L"out"));
    EXPECT(Same(StateName(SeatState::Awaiting), L"awaiting"));
    EXPECT(Same(StateName(SeatState::Rejoining), L"rejoining"));
    EXPECT(Same(StateName(SeatState::In), L"in"));
    EXPECT(Same(StateName(SeatState::Playing), L"playing"));
    EXPECT(Same(StateName(SeatState::Handed), L"handed"));
    for (int s = 0; s <= static_cast<int>(SeatState::Handed); ++s)  // every word has a meaning for the log
    {
        EXPECT(!Same(lss::players::StateMeaning(static_cast<SeatState>(s)), L"?"));
    }
    static_assert(StateOf({.in = true, .playsMember = true}) == SeatState::Playing, "constexpr");
}

}  // namespace

int main()
{
    TestOutOfTheGame();
    TestInTheGame();
    TestInGame();
    TestWords();
    return lss::test::Report();
}
