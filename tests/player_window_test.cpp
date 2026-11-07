// Host-side tests for the player windows' pure rules and texts (session/player_window_rules.h,
// session/player_window_text.h): the keys in a player's own window, a pad that presses on the title screen taking
// player 1, a joining pad taking the new player's slot, each window's lines and the character's name.

#include "session/player_window_rules.h"
#include "session/player_window_text.h"

#include "expect.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace lss::session::player_window_rules;
using namespace lss::session::player_window_text;
namespace pad = lss::input::pad_rules;

bool Has(const std::wstring& text, const std::wstring& part)
{
    return text.find(part) != std::wstring::npos;
}

/// The markup's lines, split at the game's CR LF.
std::vector<std::wstring> LinesOf(const std::wstring& markup)
{
    std::vector<std::wstring> lines;
    std::size_t               from = 0;
    for (std::size_t at = markup.find(L"\r\n"); at != std::wstring::npos; at = markup.find(L"\r\n", from))
    {
        lines.push_back(markup.substr(from, at - from));
        from = at + 2;
    }
    lines.push_back(markup.substr(from));
    return lines;
}

/// A and Start close an open window; any other press does nothing (B's tap and hold come by OnB).
void TheKeys()
{
    EXPECT(OnPress(pad::kA).act == Act::Close);
    EXPECT(OnPress(pad::kStart).act == Act::Close);
    EXPECT(OnPress(pad::kB).act == Act::Nothing);
    EXPECT(OnPress(pad::kX).act == Act::Nothing);
}

/// B: a tap closes, a hold leaves (player 1 refused), nothing yet does nothing.
void TheB()
{
    EXPECT(OnB(1, true, false).act == Act::Close);
    EXPECT(OnB(0, true, false).act == Act::Close);
    EXPECT(OnB(2, false, true).act == Act::Leave);
    EXPECT(OnB(0, false, true).act == Act::Refused);
    EXPECT(OnB(1, false, false).act == Act::Nothing);
}
/// The title screen's press takes player 1's slot; a join's pad takes the new player's; a pad's seat is its slot's.
void PadsAndSeats()
{
    EXPECT((WithPlayerOne({0, 1, 2, 3}, 2) == pad::PadTable{2, 1, 0, 3}));
    EXPECT((WithPlayerOne({0, 1, 2, 3}, 0) == pad::kIdentity));  // already player 1's
    EXPECT((WithPadOn({0, 1, 2, 3}, 3, 1) == pad::PadTable{0, 3, 2, 1}));
    EXPECT(pad::IsPermutation(WithPadOn({2, 0, 3, 1}, 1, 2)));
    EXPECT(SeatOf({0, 1, 2, 3}, 0b1, 0) == 0);
    EXPECT(SeatOf({0, 1, 2, 3}, 0b1, 1) == -1);  // player 2 not in: the pad plays no one
    EXPECT(SeatOf({0, 1, 2, 3}, 0b11, 1) == 1);
    EXPECT(SeatOf({0, 1, 2, 3}, 0b111, 3) == -1);  // the spare slot
    EXPECT(SeatOf({1, 0, 2, 3}, 0b1, 1) == 0);
}

/// Player 2's open window: the character, done, leave; one key a line.
void PlayerTwosWindow()
{
    Window w{.seat = 1, .name = L"BARRET", .open = true};
    const ScreenText t = TextFor(w);
    const std::vector<std::wstring> lines{
        L"PLAYER 2    <color=yellow>BARRET</color>",
        L"<button=cross,pad> Done",
        L"Hold <button=circle,pad> to leave the game",
    };
    EXPECT(LinesOf(t.markup) == lines);
    EXPECT(t.plain[0] == L"PLAYER 2    BARRET");
    EXPECT(t.plain[1] == L"A: done");
    EXPECT(t.plain[2] == L"Hold B to leave the game");
    EXPECT(t.plain[3].empty());
}
/// Player 1's: no leave; the Start hint while a player can still join.
void PlayerOnesWindow()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = true, .startJoins = true};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"<button=cross,pad> Done",
        L"Press <button=options,pad> on another controller to join",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.startJoins = false;
    EXPECT(!Has(TextFor(w).markup, L"options"));
    EXPECT(!Has(TextFor(w).markup, L"leave"));
}

/// A note replaces the keys; with the window shut it shows alone under the name.
void Notes()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = false, .note = Note::Waits, .about = 1};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"Player 2 joins once a party member is free",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.open = true;
    w.note = Note::PlayerOneStays;
    EXPECT(LinesOf(TextFor(w).markup).back() == L"Player 1 cannot be removed from the game");
    EXPECT(!Has(TextFor(w).markup, L"Done"));
    EXPECT(NoteText(Note::AllIn, 0) == L"Every player is in already");
}

/// The character's name from a party character's object or class name; any other name gives none.
void CharacterNames()
{
    EXPECT(CharacterName(L"PC0005_00_Yuffie_Standard_C_2147482") == L"YUFFIE");
    EXPECT(CharacterName(L"PC0000_00_Cloud_Standard_C") == L"CLOUD");
    EXPECT(CharacterName(L"PC0000_00_Cloud_Standard_AIBP_C") == L"CLOUD");
    EXPECT(CharacterName(L"BP_Enemy_C").empty());
    EXPECT(CharacterName(L"PC0000_00").empty());
    EXPECT(CharacterName(L"PC0000__x").empty());
}

}  // namespace

int main()
{
    TheKeys();
    TheB();
    PadsAndSeats();
    PlayerTwosWindow();
    PlayerOnesWindow();
    Notes();
    CharacterNames();
    return lss::test::Report();
}
