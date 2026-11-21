// Host-side tests for each pad's pure input rules (input/pad_rules.h). Run without the game.

#include "input/pad_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::input::pad_rules;

/// Each reader's lanes hold its own pads once each; a pad's slot is found.
void TheTable()
{
    EXPECT(IsValid(kIdentity));
    EXPECT(IsValid(FromXInput({2, 0, 1, 3})));
    EXPECT(!IsValid(FromXInput({0, 0, 1, 3})));
    EXPECT(!IsValid(FromXInput({0, 1, 2, 4})));  // a PlayStation pad in an XInput lane
    PadTable live = kIdentity;
    live.live[3]  = kKeyboard;
    EXPECT(!IsValid(live));  // the keyboard never on the spare slot
    live.live[3]  = 3;
    EXPECT(!IsValid(live));  // no such reader
    live.live[3]  = kXInput;
    live.live[1]  = kKeyboard;
    EXPECT(IsValid(live) && KeyboardSlot(live) == 1 && PadOn(live, 1) == kNoPad);  // the keyboard plays player 2
    EXPECT(SlotOf(FromXInput({2, 0, 1, 3}), 0) == 1);
    EXPECT(SlotOf(FromXInput({2, 0, 1, 3}), 3) == 3);
    EXPECT(SlotOf(kIdentity, 5) == 1);  // the second PlayStation pad, on record 1
    EXPECT(PadOn(kIdentity, 2) == 2);   // XInput live
}

/// A pad put on a slot swaps, in its own reader, with the pad there, and its reader becomes the slot's live one.
void WithPadOnSwaps()
{
    const PadTable t = WithPadOn(kIdentity, 4, 1);  // the first PlayStation pad onto player 2's slot
    EXPECT(IsValid(t));
    EXPECT(t.lanes[Lane(kPlayStation, 1)] == 4 && t.lanes[Lane(kPlayStation, 0)] == 5);
    EXPECT(t.lanes[Lane(kXInput, 1)] == 1);  // XInput's lanes untouched
    EXPECT(PadOn(t, 1) == 4 && PadOn(t, 0) == 0);
    const PadTable same = WithPadOn(kIdentity, 4, 0);  // already on the slot: only the live reader changes
    EXPECT(same.lanes == kIdentity.lanes && PadOn(same, 0) == 4);
    EXPECT(WithPadOn(kIdentity, 2, 4) == kIdentity);  // no such slot
}

/// The game's last-device note after a table change: player 1's new device noted as its press would note it (an XInput
/// slot at once, a PlayStation record at its read); a noted pad that moved, followed to its new slot.
void TheNoteFollows()
{
    const DeviceNote xinputOne{kClassXInput, 0, 0};
    const DeviceNote sonyOne{kClassPlayStation, 0, 0};
    // Pad 1 on player 1, the first PlayStation pad on player 2; they trade: the pad's note follows it to slot 1 now,
    // the PlayStation pad is noted at record 0's read.
    const auto is = [](const NoteChange& c, bool newDevice, NoteFlag flag, int xinputSlot, int sonySlot) {
        return c.newDevice == newDevice && c.flag == flag && c.xinputSlot == xinputSlot && c.sonySlot == sonySlot;
    };
    const PadTable xbox = WithPadOn(kIdentity, 4, 1);
    const PadTable ps   = WithPadOn(WithPadOn(xbox, 4, 0), 0, 1);
    EXPECT(PadOn(ps, 0) == 4 && PadOn(ps, 1) == 0);
    EXPECT(is(Renoted(xbox, ps, xinputOne), true, NoteFlag::None, 1, 0));
    // Traded back: pad 1 noted on slot 0 at once, flag raised; the PlayStation record moves before the Tick reads it.
    EXPECT(is(Renoted(ps, xbox, sonyOne), true, NoteFlag::XInput, 0, -1));
    // Both in one frame (the Tick not run between): the second change is a new device too, so the first one's XInput
    // flag is lowered and the PlayStation pad's note waits for record 0.
    EXPECT(is(Renoted(xbox, ps, sonyOne), true, NoteFlag::None, -1, 0));
    // The keyboard takes player 1 from the PlayStation pad: the keyboard's flag, a noted pad that stays put kept.
    PadTable kb = ps;
    kb.live[1]  = kKeyboard;
    const PadTable kbOne = WithPadOn(WithKeyboardOn(kb, 0), 4, 1);
    EXPECT(KeyboardSlot(kbOne) == 0 && PadOn(kbOne, 1) == 4);
    EXPECT(is(Renoted(kb, kbOne, DeviceNote{kClassXInput, 2, 0}), true, NoteFlag::Keyboard, -1, -1));
    // From the keyboard to a PlayStation pad, and to an XInput pad (the same lane, only the live reader changes).
    EXPECT(is(Renoted(kbOne, kb, DeviceNote{kClassKeyboard, 0, 0}), true, NoteFlag::None, -1, 0));
    const PadTable kbOnPadLane = WithKeyboardOn(kIdentity, 0);
    EXPECT(is(Renoted(kbOnPadLane, kIdentity, DeviceNote{kClassKeyboard, 0, 0}), true, NoteFlag::XInput, 0, -1));
    // One PlayStation pad for another on player 1: noted at record 0's read; the noted record moving is not followed.
    const PadTable sonyA = WithPadOn(kIdentity, 4, 0);
    const PadTable sonyB = WithPadOn(sonyA, 5, 0);
    EXPECT(PadOn(sonyB, 0) == 5);
    EXPECT(is(Renoted(sonyA, sonyB, sonyOne), true, NoteFlag::None, -1, 0));
    // Players 2 and 3 trade while player 1 keeps the PlayStation pad: a noted XInput pad followed to its new slot.
    const PadTable two   = WithPadOn(kIdentity, 4, 0);
    const PadTable three = WithPadOn(WithPadOn(two, 2, 1), 1, 2);
    EXPECT(PadOn(three, 1) == 2 && PadOn(three, 2) == 1);
    EXPECT(is(Renoted(two, three, DeviceNote{kClassXInput, 1, 0}), false, NoteFlag::None, 2, -1));
    EXPECT(is(Renoted(two, three, DeviceNote{kClassXInput, 0, 0}), false, NoteFlag::None, -1, -1));  // nothing moved
    // ... and a noted PlayStation pad, at its new record's read.
    const PadTable sonyTwo   = WithPadOn(WithPadOn(kIdentity, 4, 1), 5, 2);
    const PadTable sonyThree = WithPadOn(WithPadOn(sonyTwo, 5, 1), 4, 2);
    EXPECT(PadOn(sonyThree, 1) == 5 && PadOn(sonyThree, 2) == 4);
    EXPECT(is(Renoted(sonyTwo, sonyThree, DeviceNote{kClassPlayStation, 0, 1}), false, NoteFlag::None, -1, 2));
    // No change, the keyboard or DirectInput noted, or indices the game never writes: nothing.
    EXPECT(is(Renoted(xbox, xbox, xinputOne), false, NoteFlag::None, -1, -1));
    EXPECT(is(Renoted(two, three, DeviceNote{}), false, NoteFlag::None, -1, -1));
    EXPECT(is(Renoted(two, three, DeviceNote{3, 1, 1}), false, NoteFlag::None, -1, -1));
    EXPECT(is(Renoted(two, three, DeviceNote{kClassXInput, -1, 7}), false, NoteFlag::None, -1, -1));
    EXPECT(is(Renoted(sonyTwo, sonyThree, DeviceNote{kClassPlayStation, 0, 7}), false, NoteFlag::None, -1, -1));
    // A note waiting for its record's read, through a second change before that read.
    const NoteChange none{};
    const NoteChange newOne{true, NoteFlag::XInput, 0, -1};
    const NoteChange follows{false, NoteFlag::None, -1, 2};
    EXPECT(NextWaiting(0, none, true) == 0);       // players 2 and 3 trade: player 1's PlayStation pad still waits
    EXPECT(NextWaiting(0, follows, true) == 0);    // ... over a record named from the stale index
    EXPECT(NextWaiting(0, newOne, false) == -1);   // player 1 back on an XInput pad: nothing waits
    EXPECT(NextWaiting(2, none, false) == 2);      // another record's note kept
    EXPECT(NextWaiting(1, follows, false) == 2);   // ... or replaced by the one the change names
    EXPECT(NextWaiting(-1, none, true) == -1);
    EXPECT(NextWaiting(-1, NoteChange{true, NoteFlag::None, -1, 0}, true) == 0);
}

/// A pad beside the one that plays a player's slot is muted while that one answers; on a slot no player holds, or
/// beside an absent pad, it plays on.
void TheMute()
{
    constexpr std::uint32_t kBoth   = 0b0001'0001;  // pad 1 and the first PlayStation pad answer
    constexpr std::uint32_t kPlayer = 0b1;          // player 1 alone
    EXPECT(Muted(kIdentity, kPlayer, kBoth, 4));    // XInput live on slot 0: the PlayStation pad is muted
    EXPECT(!Muted(kIdentity, kPlayer, kBoth, 0));
    EXPECT(!Muted(kIdentity, kPlayer, 0b0001'0000, 4));  // pad 1 absent: the PlayStation pad plays player 1
    const PadTable ps = WithPadOn(kIdentity, 4, 0);
    EXPECT(Muted(ps, kPlayer, kBoth, 0) && !Muted(ps, kPlayer, kBoth, 4));
    EXPECT(!Muted(kIdentity, kPlayer, 0b0010'0010, 5));  // slot 1, no player 2: both pads play on (Start gated)
    EXPECT(Muted(kIdentity, 0b11, 0b0010'0010, 5));
    EXPECT(MutedPads(kIdentity, kPlayer, kBoth) == 0b0001'0000);
    EXPECT(!Muted(kIdentity, kPlayer, 0b0000'0001, 4));  // a PlayStation pad that does not answer is not muted
    // The keyboard on player 2's slot mutes both of its pads that answer (the keyboard is always there).
    const PadTable kb = WithKeyboardOn(kIdentity, 1);
    EXPECT(Muted(kb, 0b11, 0b0010'0010, 1) && Muted(kb, 0b11, 0b0010'0010, 5));
    EXPECT(!Muted(kb, 0b01, 0b0010'0010, 1));  // player 2 not in: no one's slot
    EXPECT(!Muted(kb, 0b11, 0b0000'0000, 1));  // absent: not muted
    EXPECT(MutedPads(kb, 0b11, 0b0011'0011) == 0b0011'0010);  // and the PlayStation pad beside player 1's
}

/// A key event is the keyboard's when it has a key code; a pad button's event has none.
void KeyboardKeys()
{
    EXPECT(IsKeyboardKey(kOpenKey) && IsKeyboardKey(kLeaveKey) && IsKeyboardKey(0x57));
    EXPECT(!IsKeyboardKey(0) && !IsKeyboardKey(256));
}

/// One pad read by both readers: presses within kTwinMs are one; a twin's Start joins only as the PlayStation copy,
/// and never when the other copy plays a player.
void TheTwins()
{
    EXPECT(Twins(1000, 1000) && Twins(1000, 1000 + kTwinMs) && Twins(1000 + kTwinMs, 1000));
    EXPECT(!Twins(1000, 1001 + kTwinMs));  // two people a poll's reach apart
    EXPECT(!Twins(0, 0) && !Twins(1000, 0));  // a pad that never pressed it
    EXPECT(TwinStartDropped(1, false));   // both copies spare: the XInput copy's Start is dropped...
    EXPECT(!TwinStartDropped(4, false));  // ...and the PlayStation copy joins
    EXPECT(TwinStartDropped(4, true) && TwinStartDropped(0, true));  // the other copy plays: the Start is its player's
}

/// The PlayStation buttons as the rules' XInput bits, and the rules' answer back as PlayStation buttons.
void PlayStationButtons()
{
    EXPECT(FromPlayStation(0x8) == kStart);
    EXPECT(FromPlayStation(kCreate) == kView);    // the opener, left of the centre as View is
    EXPECT(FromPlayStation(0x100000) == 0);       // the touchpad: the game's map button, not held back
    EXPECT(FromPlayStation(0x4000 | 0x2000) == (kA | kB));
    EXPECT(FromPlayStation(0x100 | 0x200 | 0x10000 | 0x200000) == 0);  // L2, R2 digital; PS and Mute
    EXPECT(ToPlayStation(kStart | kY) == (0x8u | 0x1000u));
    for (const ButtonPair& b : kPlayStationButtons)
    {
        EXPECT(FromPlayStation(b.playStation) == b.xinput && ToPlayStation(b.xinput) == b.playStation);
    }
    // Start held back (a spare pad), L2 kept as it is.
    EXPECT(PlayStationShown(0x8 | 0x4000 | 0x100, kStart | kA, kA) == (0x4000u | 0x100u));
    // The opener's tap at its let-go is Create, which the game never sees; Cross kept.
    EXPECT(PlayStationShown(0x4000, kA, kA | kView) == 0x4000u);
    // Create held back, the touchpad and the system buttons: the touchpad through, the rest never.
    EXPECT(PlayStationShown(kCreate | 0x100000 | 0x10000 | 0x200000, kView, 0) == 0x100000u);
    EXPECT(PlayStationShown(0x10000 | 0x4000, kA, kA) == 0x4000u);  // PS alone, no rule changed: still taken out
}

/// B is a tap or a hold, never both: a let-go within a second is the tap (at the let-go); a second down is the hold,
/// once, again only after a let-go, and that let-go is no tap.
void TheLeaveHold()
{
    Held h{};
    EXPECT(Hold(h, false, 0, kLeaveHoldMs) == Long::None);  // never down: no tap
    EXPECT(Hold(h, true, 0, kLeaveHoldMs) == Long::None);   // the press itself says nothing yet
    EXPECT(Hold(h, true, kLeaveHoldMs - 1, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, true, kLeaveHoldMs, kLeaveHoldMs) == Long::Hold);
    EXPECT(Hold(h, true, kLeaveHoldMs + 500, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, false, kLeaveHoldMs + 600, kLeaveHoldMs) == Long::None);  // the hold's let-go is no tap
    EXPECT(Hold(h, true, 5000, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, true, 5000 + kLeaveHoldMs, kLeaveHoldMs) == Long::Hold);
    Held t{};
    EXPECT(Hold(t, true, 0, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(t, true, 200, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(t, false, 250, kLeaveHoldMs) == Long::Tap);  // let go within the second: the tap, once
    EXPECT(Hold(t, false, 300, kLeaveHoldMs) == Long::None);
}

/// A pad on a slot no player holds never pauses the game; player 1's slot always does.
void StartOnlyForPlayers()
{
    EXPECT(StartGate(kStart | kA, 0, 0b1) == (kStart | kA));
    EXPECT(StartGate(kStart | kA, 0, 0) == (kStart | kA));       // slot 0 whatever the mask
    EXPECT(StartGate(kStart | kA, 1, 0b1) == kA);                // no player 2: Start held back, A passes
    EXPECT(StartGate(kStart | kA, 1, 0b11) == (kStart | kA));
    EXPECT(StartGate(kStart, 3, 0b111) == 0);                    // the spare slot
}

/// View held a second opens a window; a shorter View press reaches the game at its let-go; other buttons pass.
void TheOpener()
{
    OpenHold h{};
    Gated    g = Gate(h, kView | kA, 0);
    EXPECT(g.buttons == kA && !g.open);  // View held back, A passes
    g = Gate(h, kView, kOpenHoldMs - 1);
    EXPECT(g.buttons == 0 && !g.open);
    g = Gate(h, kView, kOpenHoldMs);
    EXPECT(g.buttons == 0 && g.open);    // fires once
    g = Gate(h, kView, kOpenHoldMs + 500);
    EXPECT(g.buttons == 0 && !g.open);   // hidden until let go
    g = Gate(h, 0, kOpenHoldMs + 600);
    EXPECT(g.buttons == 0 && !g.open);   // the fired press is never a tap

    // A tap: the game gets View on the let-go poll, then up on the next.
    h = {};
    EXPECT(Gate(h, kView, 0).buttons == 0);
    EXPECT(Gate(h, kView, 90).buttons == 0);
    EXPECT(Gate(h, 0, 110).buttons == kView);
    EXPECT(Gate(h, 0, 126).buttons == 0);

    // Menu (Start) and the rest are never held back.
    h = {};
    EXPECT(Gate(h, kB | 0x0010, 0).buttons == (kB | 0x0010));

    // A new press after a fired one starts over.
    h = {};
    Gate(h, kView, 0);
    EXPECT(Gate(h, kView, kOpenHoldMs).open);
    Gate(h, 0, kOpenHoldMs + 10);
    EXPECT(!Gate(h, kView, 5000).open);
    EXPECT(Gate(h, kView, 5000 + kOpenHoldMs).open);
}
/// After a change a pad stays idle until it lets go of what it held then; a resting trigger or a stuck button never
/// holds it for good.
void TheLetGo()
{
    LetGo w{};
    EXPECT(Poll(w, kA, 0, 0, 0) == Wait::None);  // not waiting

    AwaitLetGo(w);
    EXPECT(Poll(w, kA, 0, 0, 0) == Wait::Holding);        // the press that closed a window
    EXPECT(Poll(w, kA | kB, 0, 0, 50) == Wait::Holding);  // a new press does not extend it
    EXPECT(Poll(w, kB, 0, 0, 100) == Wait::LetGoNow);     // A up: free (idle on this poll still)
    EXPECT(Poll(w, kB, 0, 0, 120) == Wait::None);

    AwaitLetGo(w);
    EXPECT(Poll(w, 0, 0, 0, 0) == Wait::LetGoNow);  // nothing held: free at once

    // A trigger pulled at the change lets go below the threshold, or when it drops by the threshold.
    AwaitLetGo(w);
    EXPECT(Poll(w, 0, 255, 0, 0) == Wait::Holding);
    EXPECT(Poll(w, 0, 240, 0, 10) == Wait::Holding);
    EXPECT(Poll(w, 0, 200, 0, 20) == Wait::LetGoNow);

    // A worn trigger resting at 60: it never drops; 2 s after the buttons let go the pad is free anyway.
    AwaitLetGo(w);
    EXPECT(Poll(w, kA, 60, 0, 0) == Wait::Holding);
    EXPECT(Poll(w, 0, 60, 0, 100) == Wait::Holding);
    EXPECT(Poll(w, 0, 61, 0, 100 + kLetGoCapMs - 1) == Wait::Holding);
    EXPECT(Poll(w, 0, 60, 0, 100 + kLetGoCapMs) == Wait::TriggerRests);
    EXPECT(Poll(w, 0, 60, 0, 100 + kLetGoCapMs + 16) == Wait::None);

    // A stuck button bit, down at the change and never up: free 10 s after the change.
    AwaitLetGo(w);
    EXPECT(Poll(w, kY, 0, 0, 1000) == Wait::Holding);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs - 1) == Wait::Holding);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs) == Wait::ButtonStuck);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs + 16) == Wait::None);
}

}  // namespace

int main()
{
    TheTable();
    WithPadOnSwaps();
    TheNoteFollows();
    TheMute();
    KeyboardKeys();
    PlayStationButtons();
    TheTwins();
    TheOpener();
    TheLetGo();
    TheLeaveHold();
    StartOnlyForPlayers();
    return lss::test::Report();
}
