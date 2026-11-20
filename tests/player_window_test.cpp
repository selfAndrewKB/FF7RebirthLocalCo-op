// Host-side tests for the player windows' pure rules and texts (session/player_window_rules.h,
// session/player_window_text.h): the keys in a player's own window, the player numbers traded with each person keeping
// their pad and screen, a spare pad taking back a player whose pad is gone, a pad that presses on the title screen
// taking player 1, a joining pad taking the new player's slot, each window's lines and the character's name.

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

/// The d-pad's left/right trade numbers, LB/RB move the view, A and Start close an open window; any other press does
/// nothing (B's tap and hold come by OnB; the d-pad's up/down choose no character here).
void TheKeys()
{
    EXPECT(OnPress(kDpadLeft) == (Action{Act::Trade, -1}));
    EXPECT(OnPress(kDpadRight) == (Action{Act::Trade, +1}));
    EXPECT(OnPress(0x0001).act == Act::Nothing);  // up
    EXPECT(OnPress(0x0002).act == Act::Nothing);  // down
    EXPECT(OnPress(kLB) == (Action{Act::Move, -1}));
    EXPECT(OnPress(kRB) == (Action{Act::Move, +1}));
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
/// A view moves one screen, swapped with what is there (a player or a copy); never past the row's edge.
void TheViewMoves()
{
    const auto right = Moved({0, 1, -1}, 3, 0, +1);
    EXPECT(right && (*right == std::array<int, kSeats>{1, 0, -1}));
    const auto ontoCopy = Moved({0, 1, -1}, 3, 1, +1);
    EXPECT(ontoCopy && (*ontoCopy == std::array<int, kSeats>{0, -1, 1}));
    EXPECT(!Moved({0, 1, -1}, 3, 0, -1));   // the left edge
    EXPECT(!Moved({0, 1, 2}, 3, 2, +1));    // the right edge
    EXPECT(!Moved({0, -1, -1}, 1, 0, +1));  // one screen
    EXPECT(!Moved({0, 1, -1}, 2, 2, -1));   // a seat on no screen
    const auto two = Moved({1, 0, -1}, 2, 0, -1);
    EXPECT(two && (*two == std::array<int, kSeats>{0, 1, -1}));
    EXPECT(OrderWord({2, 1, 3}) == L"213");
}

/// A number trade goes to the nearest player that way, stopping at the ends; the two screens trade numbers, so each
/// person keeps their screen whichever came first, the move or the trade; the two pads trade slots.
void NumbersTrade()
{
    EXPECT(TradePartner(0, +1, 0b011) == 1);
    EXPECT(TradePartner(1, -1, 0b011) == 0);
    EXPECT(TradePartner(0, -1, 0b111) == -1);  // the lowest number
    EXPECT(TradePartner(2, +1, 0b111) == -1);  // the highest
    EXPECT(TradePartner(1, +1, 0b011) == -1);  // player 3 not in
    EXPECT(TradePartner(0, +1, 0b101) == 2);   // player 2 not in: the next player that way
    EXPECT(TradePartner(2, -1, 0b101) == 0);
    EXPECT(TradePartner(0, +1, 0b001) == -1);  // alone
    EXPECT(TradePartner(1, 0, 0b111) == -1);

    // Player 2 moved their view onto the left screen, then trades with player 1: the left screen shows player 1 (them).
    const auto moved = Moved({0, 1, -1}, 2, 1, -1);
    EXPECT(moved && (*moved == std::array<int, kSeats>{1, 0, -1}));
    EXPECT((Traded(*moved, 1, 0) == std::array<int, kSeats>{0, 1, -1}));
    // Three screens with a copy's: only the two traded numbers change places.
    EXPECT((Traded({2, -1, 0}, 0, 2) == std::array<int, kSeats>{0, -1, 2}));
    EXPECT((Traded({0, 1, 2}, 1, 2) == std::array<int, kSeats>{0, 2, 1}));
    EXPECT((Traded({0, 1, -1}, 1, 1) == std::array<int, kSeats>{0, 1, -1}));  // with itself: nothing
    // The pads: player 2's pad onto player 1's slot, player 1's onto player 2's.
    const pad::PadTable pads = pad::FromXInput({2, 0, 1, 3});
    EXPECT((WithPadsTraded(pads, 0, 1) == pad::FromXInput({0, 2, 1, 3})));
    EXPECT(!Plays(0b111, 3) && !Plays(0b111, -1) && Plays(0, 0));

    // Every person keeps their pad and their screen through any trade: three players, each pad's screen before and
    // after; once with XInput pads only, once with player 2 on a PlayStation pad.
    const std::array<int, kSeats> seatOn{2, 0, 1};
    const auto shown = [](const pad::PadTable& t, const std::array<int, kSeats>& on, int s) {
        return pad::PadOn(t, on[static_cast<std::size_t>(s)]);  // the pad whose player a screen shows
    };
    for (const pad::PadTable& before : {pad::FromXInput({1, 3, 0, 2}), WithPadOn(pad::FromXInput({1, 3, 0, 2}), 6, 1)})
    {
        for (int seat = 0; seat < kSeats; ++seat)
        {
            for (const int dir : {-1, +1})
            {
                const int other = TradePartner(seat, dir, 0b111);
                if (other < 0)
                {
                    continue;
                }
                const pad::PadTable           after   = WithPadsTraded(before, seat, other);
                const std::array<int, kSeats> screens = Traded(seatOn, seat, other);
                EXPECT(pad::IsValid(after));
                for (int screen = 0; screen < kSeats; ++screen)
                {
                    EXPECT(shown(after, screens, screen) == shown(before, seatOn, screen));
                }
            }
        }
    }
}

/// A spare pad's Start takes back the lowest player whose pad is gone; none gone, or gone pads nobody plays: -1. (A slot
/// no pad ever answered, a keyboard player 1's, is never in the gone set: input/pad_presence.)
void PadsGone()
{
    const pad::PadTable id = pad::kIdentity;
    EXPECT(SeatWithPadGone(id, 0b011, 0b0010) == 1);
    EXPECT(SeatWithPadGone(id, 0b111, 0b0101) == 0);   // player 1's and player 3's: the lowest
    EXPECT(SeatWithPadGone(id, 0b011, 0b1100) == -1);  // gone pads no player holds
    EXPECT(SeatWithPadGone(pad::FromXInput({3, 1, 2, 0}), 0b001, 0b1000) == 0);  // player 1 on pad 4
    EXPECT(SeatWithPadGone(id, 0b111, 0) == -1);
    // Player 2 on the first PlayStation pad: its going counts, pad 2's (muted beside it) does not.
    const pad::PadTable ps = WithPadOn(id, 4, 1);
    EXPECT(SeatWithPadGone(ps, 0b011, 0b0001'0000) == 1);
    EXPECT(SeatWithPadGone(ps, 0b011, 0b0000'0010) == -1);
}

/// The title screen's press takes player 1's slot; a join's pad takes the new player's; a pad's seat is its slot's,
/// unless it is muted there.
void PadsAndSeats()
{
    const pad::PadTable id  = pad::kIdentity;
    constexpr std::uint32_t kAll = 0xFF;
    EXPECT((WithPlayerOne(id, 2) == pad::FromXInput({2, 1, 0, 3})));
    EXPECT((WithPlayerOne(id, 0) == pad::kIdentity));  // already player 1's
    EXPECT((WithPadOn(id, 3, 1) == pad::FromXInput({0, 3, 2, 1})));
    EXPECT(pad::IsValid(WithPadOn(pad::FromXInput({2, 0, 3, 1}), 1, 2)));
    EXPECT(SeatOf(id, 0b1, kAll, 0) == 0);
    EXPECT(SeatOf(id, 0b1, kAll, 1) == -1);  // player 2 not in: the pad plays no one
    EXPECT(SeatOf(id, 0b11, kAll, 1) == 1);
    EXPECT(SeatOf(id, 0b111, kAll, 3) == -1);  // the spare slot
    EXPECT(SeatOf(pad::FromXInput({1, 0, 2, 3}), 0b1, kAll, 1) == 0);
    // An Xbox pad and a DualSense on player 1's slot: the Xbox pad plays, the DualSense is muted and its Start joins;
    // with the Xbox pad gone, the DualSense plays player 1.
    EXPECT(SeatOf(id, 0b1, 0b0001'0001, 4) == -1 && SeatOf(id, 0b1, 0b0001'0001, 0) == 0);
    EXPECT(SeatOf(id, 0b1, 0b0001'0000, 4) == 0);
    // The DualSense pressed on the title: it plays player 1, the Xbox pad is muted.
    const pad::PadTable title = WithPlayerOne(id, 4);
    EXPECT(SeatOf(title, 0b1, 0b0001'0001, 4) == 0 && SeatOf(title, 0b1, 0b0001'0001, 0) == -1);
    // The muted DualSense joins as player 2: on record 1, live there; player 1 stays the Xbox pad's.
    const pad::PadTable joined = WithPadOn(id, 4, 1);
    EXPECT(SeatOf(joined, 0b11, 0b0001'0001, 4) == 1 && SeatOf(joined, 0b11, 0b0001'0001, 0) == 0);
}

/// A key on the title makes the keyboard player 1: every pad on player 1's slot plays no one (its Start joins); a
/// joined pad plays its own player; the keyboard is never gone; a pad's title press takes player 1 back. In a window
/// the keyboard trades numbers as a pad does: the pad takes the keyboard's number and the keyboard the pad's.
void KeyboardPlayerOne()
{
    const pad::PadTable kb = pad::WithKeyboardOn(pad::kIdentity, 0);
    EXPECT(pad::IsValid(kb) && pad::KeyboardSlot(kb) == 0 && pad::PadOn(kb, 0) == pad::kNoPad);
    EXPECT(SeatOf(kb, 0b1, 0b0001'0001, 0) == -1 && SeatOf(kb, 0b1, 0b0001'0001, 4) == -1);
    EXPECT(SeatOf(kb, 0b1, 0b0000'0000, 0) == -1);  // absent: not muted, but no player's either
    const pad::PadTable joined = WithPadOn(kb, 0, 1);  // pad 1's Start adds player 2
    EXPECT(pad::KeyboardSlot(joined) == 0 && SeatOf(joined, 0b11, 0b1, 0) == 1);
    EXPECT(SeatWithPadGone(joined, 0b11, 0xFF) == 1);  // only player 2's pad can be gone
    EXPECT(pad::KeyboardSlot(WithPlayerOne(kb, 4)) == -1 && pad::PadOn(WithPlayerOne(kb, 4), 0) == 4);
    EXPECT(pad::PadName(pad::kNoPad) == L"the keyboard" && TitleClaimed(pad::kNoPad) == L"Keyboard is set as Player 1");

    // Player 2's pad trades with the keyboard's player 1: pad 1 plays player 1, the keyboard player 2; and back.
    EXPECT(CanTradePads(joined, 1, 0) && CanTradePads(joined, 0, 1));
    const pad::PadTable traded = WithPadsTraded(joined, 1, 0);
    EXPECT(pad::IsValid(traded) && pad::PadOn(traded, 0) == 0 && pad::KeyboardSlot(traded) == 1);
    EXPECT(SeatOf(traded, 0b11, 0b1, 0) == 0 && pad::PadOn(traded, 1) == pad::kNoPad);
    EXPECT(WithPadsTraded(traded, 0, 1) == joined);
    // A PlayStation pad on player 3 trades with the keyboard on player 2.
    const pad::PadTable three = WithPadOn(traded, 6, 2);
    const pad::PadTable psTraded = WithPadsTraded(three, 2, 1);
    EXPECT(pad::IsValid(psTraded) && pad::PadOn(psTraded, 1) == 6 && pad::KeyboardSlot(psTraded) == 2);
    EXPECT(!CanTradePads(joined, 1, 1) && !CanTradePads(joined, 0, 3));

    // The keyboard plays one player slot at most, never the spare.
    pad::PadTable two = pad::kIdentity;
    two.live[0]       = pad::kKeyboard;
    two.live[1]       = pad::kKeyboard;
    EXPECT(!pad::IsValid(two));
    pad::PadTable spare = pad::kIdentity;
    spare.live[3]       = pad::kKeyboard;
    EXPECT(!pad::IsValid(spare) && pad::WithKeyboardOn(pad::kIdentity, 3) == pad::kIdentity);
    // Moving the keyboard frees the slot it played; Unkeyboarded puts it back beside player 1.
    const pad::PadTable moved = pad::WithKeyboardOn(kb, 2);
    EXPECT(pad::IsValid(moved) && pad::KeyboardSlot(moved) == 2 && pad::PadOn(moved, 0) == 0);
    EXPECT(pad::KeyboardSlot(pad::Unkeyboarded(moved)) == -1 && pad::Unkeyboarded(moved) == pad::kIdentity);

    // The claim: the device turning to the keyboard on the title, or the title coming up with the keyboard last used.
    EXPECT(KeyboardClaims(true, false, true, true));    // a key on the title
    EXPECT(KeyboardClaims(true, true, true, false));    // the title came up after the keyboard skipped the logos
    EXPECT(!KeyboardClaims(true, true, true, true));    // already the keyboard's: no second claim
    EXPECT(!KeyboardClaims(true, false, false, false)); // a key in the field
    EXPECT(!KeyboardClaims(false, true, true, true));   // a pad's press on the title (its own TitlePress)
}

/// Player 2's open window: the number between left and right, the character, then move, done, leave; one key a line.
void PlayerTwosWindow()
{
    Window w{.seat = 1, .name = L"BARRET", .open = true, .canTrade = true, .canMove = true};
    const ScreenText t = TextFor(w);
    const std::vector<std::wstring> lines{
        L"<button=left,pad> PLAYER 2 <button=right,pad>    <color=yellow>BARRET</color>",
        L"<button=L1,pad> <button=R1,pad> Change View",
        L"<button=cross,pad> Done",
        L"Hold <button=circle,pad> to leave the game",
    };
    EXPECT(LinesOf(t.markup) == lines);
    EXPECT(t.plain[0] == L"<  PLAYER 2  >    BARRET");
    EXPECT(t.plain[1] == L"Left/Right: player number");
    EXPECT(t.plain[2] == L"LB/RB: Change View");
    EXPECT(t.plain[3] == L"A: done");
    EXPECT(t.plain[4] == L"Hold B to leave the game");
    EXPECT(t.plain.size() == 5);  // the longest window fills the fallback's lines exactly
}
/// Player 1's: no leave; the number's arrows while another player is in; the Start hint while a player can still join.
void PlayerOnesWindow()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = true, .startJoins = true};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"<button=cross,pad> Done",
        L"Press <button=options,pad> on another controller to join",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.canTrade = true;
    EXPECT(LinesOf(TextFor(w).markup).front() == L"<button=left,pad> PLAYER 1 <button=right,pad>    <color=yellow>CLOUD</color>");
    EXPECT(TextFor(w).plain[1] == L"Left/Right: player number");
    w.open = false;  // a shut window shows no arrows
    EXPECT(TextFor(w).plain[0] == L"PLAYER 1    CLOUD");
    w.open       = true;
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
        L"Player 2 has joined and is waiting for a free party member",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.open = true;
    w.note = Note::PlayerOneStays;
    EXPECT(LinesOf(TextFor(w).markup).back() == L"Player 1 cannot be removed from the game");
    EXPECT(!Has(TextFor(w).markup, L"Done"));
    EXPECT(NoteText(Note::AllIn, 0) == L"All players are currently present");
    EXPECT(NoteText(Note::Failed, 2) == L"Player 3's join failed. See UE4SS.log.");
    // A member in the party but with the game's AI: named; without a name, the plain wait.
    EXPECT(NoteText(Note::WaitsOnAi, 1, L"SEPHIROTH") ==
           L"Player 2 has joined and is waiting for SEPHIROTH to be released from AI control");
    EXPECT(NoteText(Note::WaitsOnAi, 1) == NoteText(Note::Waits, 1));
    Window named{.seat = 0, .name = L"CLOUD", .note = Note::WaitsOnAi, .about = 2, .aboutName = L"AERITH"};
    EXPECT(LinesOf(TextFor(named).markup).back() ==
           L"Player 3 has joined and is waiting for AERITH to be released from AI control");
}

/// The keyboard as a window device: its seat is the slot it plays alone while that player is in; Tab held joins it
/// (WithDeviceOn), its keys stand for the pad's, its window names its keys.
void KeyboardWindow()
{
    EXPECT(SeatOf(pad::kIdentity, 0b11, 0b1, pad::kKeyboardPad) == -1);  // beside player 1: no player of its own
    const pad::PadTable two = WithDeviceOn(pad::kIdentity, pad::kKeyboardPad, 1);
    EXPECT(pad::KeyboardSlot(two) == 1 && SeatOf(two, 0b11, 0b11, pad::kKeyboardPad) == 1);
    EXPECT(SeatOf(two, 0b01, 0b11, pad::kKeyboardPad) == -1);  // player 2 not in yet
    EXPECT(SeatOf(two, 0b11, 0b11, 1) == -1);                   // pad 2 is muted beside the keyboard
    EXPECT(WithDeviceOn(pad::kIdentity, 4, 1) == WithPadOn(pad::kIdentity, 4, 1));
    EXPECT(pad::PadName(pad::kKeyboardPad) == L"the keyboard" && std::wstring(pad::OpenerName(pad::kKeyboardPad)) == L"Tab");
    EXPECT(std::wstring(pad::OpenerName(4)) == L"Create" && std::wstring(pad::OpenerName(0)) == L"View");
    EXPECT(pad::FromKey(0x25) == kDpadLeft && pad::FromKey(0x27) == kDpadRight && pad::FromKey(0x51) == kLB &&
           pad::FromKey(0x45) == kRB && pad::FromKey(0x0D) == pad::kA && pad::FromKey(pad::kLeaveKey) == pad::kB);
    EXPECT(pad::FromKey(0x57) == 0 && pad::FromKey(pad::kOpenKey) == 0);  // W, and Tab: the opener, never a window key
    EXPECT(OnPress(pad::FromKey(0x1B)).act == Act::Close);                // Escape closes as Start does

    Window w{.seat = 1, .name = L"TIFA", .open = true, .canTrade = true, .canMove = true, .keyboard = true};
    const std::vector<std::wstring> lines{
        L"<  PLAYER 2  >    <color=yellow>TIFA</color>",
        L"Left/Right: player number",
        L"Q/E: Change View",
        L"Enter: done",
        L"Hold Backspace to leave the game",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    EXPECT(!Has(TextFor(w).markup, L"<button="));
    // Player 1's window says Tab joins while the keyboard plays no one of its own.
    Window one{.seat = 0, .name = L"CLOUD", .open = true, .startJoins = true, .keyboardJoins = true};
    EXPECT(LinesOf(TextFor(one).markup).back() == L"Press <button=options,pad> on another controller or hold Tab on the keyboard to join");
    EXPECT(TextFor(one).plain[2] == L"Press Start on another controller or hold Tab to join");
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
    TheViewMoves();
    NumbersTrade();
    PadsGone();
    PadsAndSeats();
    KeyboardPlayerOne();
    PlayerTwosWindow();
    PlayerOnesWindow();
    KeyboardWindow();
    Notes();
    CharacterNames();
    return lss::test::Report();
}
