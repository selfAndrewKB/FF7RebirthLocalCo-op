// Host-side tests for player_two::synergy_rules, run without the game.
//
// The hand-over's answer (who the leader test names for a human's partner), the give-back's target, what the tick does
// with a member end that ran off the game thread, and the two tables' slot choice and stance dedup.

#include "player_two/battle/synergy_rules.h"

#include "expect.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace lss::player_two::synergy_rules;
using namespace std::chrono_literals;

int g_a, g_b, g_c, g_d, g_e;

void TestHandOver()
{
    std::puts("hand-over: the game's answer without a known partner, for an AI partner, a lost one or one without a "
              "type; kept through a parry window; else the partner's type, remembered");
    int lostReads = 0, typeReads = 0;
    const auto notLost = [&] { ++lostReads; return false; };
    const auto lost    = [&] { ++lostReads; return true; };
    const auto typeOf  = [&] { ++typeReads; return std::uint8_t{5}; };
    const auto noType  = [&] { ++typeReads; return kNoType; };

    auto answer = DecideHandOver(3, false, 2, notLost, typeOf, false);
    EXPECT(answer.what == HandOver::GamesOwn && answer.answer == 3);
    EXPECT(lostReads == 0 && typeReads == 0);  // nothing read

    answer = DecideHandOver(3, true, 0, notLost, typeOf, false);
    EXPECT(answer.what == HandOver::AiPartner && answer.answer == 3);
    EXPECT(lostReads == 0 && typeReads == 0);

    answer = DecideHandOver(3, true, 2, lost, typeOf, false);
    EXPECT(answer.what == HandOver::Lost && answer.answer == 3);
    EXPECT(lostReads == 1 && typeReads == 0);

    answer = DecideHandOver(3, true, 1, notLost, noType, false);
    EXPECT(answer.what == HandOver::NoType && answer.answer == 3 && answer.type == kNoType);
    EXPECT(lostReads == 2 && typeReads == 1);

    answer = DecideHandOver(3, true, 2, notLost, typeOf, true);  // the window: kept; the game's own no stands
    EXPECT(answer.what == HandOver::Kept && answer.answer == 3 && answer.type == 5);
    answer = DecideHandOver(5, true, 2, notLost, typeOf, true);  // the game's own yes waits for the counter too
    EXPECT(answer.what == HandOver::Kept && answer.answer == kNoMatch);

    answer = DecideHandOver(3, true, 2, notLost, typeOf, false);
    EXPECT(answer.what == HandOver::HandedOver && answer.answer == 5 && answer.type == 5);
    answer = DecideHandOver(5, true, 1, notLost, typeOf, false);  // the game's own yes: still handed over (remembered)
    EXPECT(answer.what == HandOver::HandedOver && answer.answer == 5);
}

void TestGiveBack()
{
    std::puts("give-back: off the game thread player 2's controller becomes player 1's; a refused one never lands on "
              "player 2; a remembered character goes to its player, player 2's member to player 2");
    auto back = DecideGiveBack(true, 2, true, true, true);
    EXPECT(back.to == GiveBackTo::PlayerOne);
    back = DecideGiveBack(true, 2, false, true, true);
    EXPECT(back.to == GiveBackTo::Original);

    back = DecideGiveBack(false, kRefused, true, true, false);
    EXPECT(back.to == GiveBackTo::PlayerOne);
    back = DecideGiveBack(false, kRefused, true, true, true);  // player 2's member: the game's own
    EXPECT(back.to == GiveBackTo::Original);
    back = DecideGiveBack(false, kRefused, true, false, false);  // no player 1
    EXPECT(back.to == GiveBackTo::Original);
    back = DecideGiveBack(false, kRefused, false, true, false);
    EXPECT(back.to == GiveBackTo::Original);

    back = DecideGiveBack(false, 1, true, true, false);
    EXPECT(back.to == GiveBackTo::Player && back.player == 1);
    back = DecideGiveBack(false, 2, false, true, true);
    EXPECT(back.to == GiveBackTo::Player && back.player == 2);
    back = DecideGiveBack(false, 0, true, true, true);  // unknown player, player 2's member: player 2
    EXPECT(back.to == GiveBackTo::Player && back.player == 2);
    back = DecideGiveBack(false, 0, true, true, false);  // unknown player, another's: no controller (the game's own)
    EXPECT(back.to == GiveBackTo::Player && back.player == 0);
}

void TestNotes()
{
    std::puts("notes: not remembered, stale, already held, the game's own give-back, or the tick's give-back");
    int reads        = 0;
    const auto holds = [&] { ++reads; return true; };
    const auto free  = [&] { ++reads; return false; };
    EXPECT(DecideNote(false, false, false, holds, false) == NoteAction::NotRemembered);
    EXPECT(DecideNote(true, true, false, holds, false) == NoteAction::Stale);
    EXPECT(DecideNote(true, false, true, holds, true) == NoteAction::Stale);
    EXPECT(reads == 0);
    EXPECT(DecideNote(true, false, false, holds, true) == NoteAction::AlreadyHeld);
    EXPECT(DecideNote(true, false, false, free, true) == NoteAction::GameGave);
    EXPECT(DecideNote(true, false, false, free, false) == NoteAction::GiveBack);
    EXPECT(reads == 3);
}

void TestSlots()
{
    std::puts("the table: a character's own slot, else a free one, else the oldest");
    std::array<Held, kHeld> held{};
    const auto              t0 = Clock::time_point(100s);
    held[0] = {&g_a, 0, 1, 0, t0 + 3s};
    held[1] = {&g_b, 0, 2, 0, t0 + 1s};  // the oldest
    held[2] = {&g_c, 0, 1, 0, t0 + 2s};
    EXPECT(&SlotFor(held, &g_a) == &held[0]);
    EXPECT(&SlotFor(held, &g_d) == &held[3]);  // free
    held[3] = {&g_d, 0, 2, 0, t0 + 4s};
    EXPECT(&SlotFor(held, &g_e) == &held[1]);  // the oldest goes
    held[2].player = 0;                        // freed: a free slot wins over the oldest
    EXPECT(&SlotFor(held, &g_e) == &held[2]);
    EXPECT(&SlotFor(held, &g_c) == &held[2]);  // a freed entry no longer names its character
}

}  // namespace

int main()
{
    TestHandOver();
    TestGiveBack();
    TestNotes();
    TestSlots();

    return lss::test::Report();
}
