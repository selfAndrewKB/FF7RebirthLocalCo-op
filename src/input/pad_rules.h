#pragma once

// The pure rules of each physical pad's input, as the pad table reads it (input/pad_table) and the player windows
// hear it (session/player_window). Host-tested (tests/pad_rules_test.cpp).
//   - the pad table: the game's pad slot N (its controller id N) is read by two readers, XInput (user N as the game
//     asks for it) and the PlayStation reader (its record N); each answers it with one of its own physical pads, a
//     permutation per reader; slot N is player N's (seat N), slot 3 the spare. Each slot's player plays on one of the
//     two pads there, the slot's live reader's; the other reader's pad there is muted while the live one is present;
//   - the window's opener, View held a second: a shorter View press still reaches the game, as a tap at its let-go;
//   - B as a tap (let go within a second) or a hold (a second down), never both;
//   - Start only on a slot a player holds: a pad that plays no one never pauses the game (its Start joins instead);
//   - the let-go: a pad that changed slots or left a window stays idle to the game until it lets go of what it held.

#include <array>
#include <cstdint>
#include <string>

namespace lss::input::pad_rules {

inline constexpr int kSlots   = 4;  // the game's pad slots (controller ids 0..3), and each reader's pads
inline constexpr int kReaders = 2;  // XInput, then the PlayStation reader
inline constexpr int kPads    = kSlots * kReaders;  // pad p: reader p / kSlots, its number there p % kSlots

inline constexpr int kXInput      = 0;  // pads 0..3: XInput users 0..3
inline constexpr int kPlayStation = 1;  // pads 4..7: the PlayStation reader's pads by scePad user 1..4
inline constexpr int kKeyboard    = 2;  // as player 1's live "reader": the keyboard plays player 1, no pad there does
inline constexpr int kNoPad       = -1;  // PadOn of a slot the keyboard plays

constexpr int ReaderOf(int pad) noexcept
{
    return pad / kSlots;
}

/// The lane of `reader` on `slot`: the table's entry for which of that reader's pads answers the slot.
constexpr int Lane(int reader, int slot) noexcept
{
    return reader * kSlots + slot;
}

/// "pad 2" for XInput user 1, "PlayStation pad 1" for the PlayStation reader's first pad, "the keyboard" for kNoPad
/// (a slot the keyboard plays): the log's and the console's names.
inline std::wstring PadName(int pad)
{
    if (pad < 0)
    {
        return L"the keyboard";
    }
    return (ReaderOf(pad) == kPlayStation ? L"PlayStation pad " : L"pad ") + std::to_wstring(pad % kSlots + 1);
}

// XInput's button bits (XINPUT_GAMEPAD_*), kept here so the rules build without Windows headers.
inline constexpr std::uint16_t kStart     = 0x0010;  // START (Menu)
inline constexpr std::uint16_t kView      = 0x0020;  // BACK
inline constexpr std::uint16_t kA         = 0x1000;
inline constexpr std::uint16_t kB         = 0x2000;
inline constexpr std::uint16_t kX         = 0x4000;
inline constexpr std::uint16_t kY         = 0x8000;

// The PlayStation reader's button bits (ScePadData.buttons, as the library fills them for the game) and the XInput
// bit each one is to the rules: Options is Start, the touchpad click is View, Cross is A, Circle is B (the keys the
// game itself sends for them). L2 and R2 have no XInput button (their analog values are the triggers); Create and the
// PS button never reach the game.
struct ButtonPair
{
    std::uint32_t playStation;
    std::uint16_t xinput;
};
inline constexpr std::array<ButtonPair, 14> kPlayStationButtons{{
    {0x000002, 0x0040},  // L3: LEFT_THUMB
    {0x000004, 0x0080},  // R3: RIGHT_THUMB
    {0x000008, kStart},  // Options
    {0x000010, 0x0001},  // up
    {0x000020, 0x0008},  // right
    {0x000040, 0x0002},  // down
    {0x000080, 0x0004},  // left
    {0x000400, 0x0100},  // L1: LEFT_SHOULDER
    {0x000800, 0x0200},  // R1: RIGHT_SHOULDER
    {0x001000, kY},      // triangle
    {0x002000, kB},      // circle
    {0x004000, kA},      // cross
    {0x008000, kX},      // square
    {0x100000, kView},   // touchpad click
}};

/// A PlayStation pad's buttons as XInput bits.
constexpr std::uint16_t FromPlayStation(std::uint32_t buttons) noexcept
{
    std::uint16_t out = 0;
    for (const ButtonPair& b : kPlayStationButtons)
    {
        out = static_cast<std::uint16_t>((buttons & b.playStation) != 0 ? out | b.xinput : out);
    }
    return out;
}

/// XInput bits as a PlayStation pad's buttons.
constexpr std::uint32_t ToPlayStation(std::uint16_t buttons) noexcept
{
    std::uint32_t out = 0;
    for (const ButtonPair& b : kPlayStationButtons)
    {
        out = (buttons & b.xinput) != 0 ? out | b.playStation : out;
    }
    return out;
}

/// The PlayStation buttons the game sees after the rules: `raw` less the XInput bits the rules held back, plus the ones
/// they added (View's tap at its let-go); L2, R2 and any bit without an XInput twin pass as they are.
constexpr std::uint32_t PlayStationShown(std::uint32_t raw, std::uint16_t rawXInput, std::uint16_t shownXInput) noexcept
{
    const auto removed = static_cast<std::uint16_t>(rawXInput & ~shownXInput);
    const auto added   = static_cast<std::uint16_t>(shownXInput & ~rawXInput);
    return (raw & ~ToPlayStation(removed)) | ToPlayStation(added);
}

/// The pad table: per lane (Lane(reader, slot)) the reader's physical pad answering the slot, and per slot its live
/// reader (whose pad there plays the slot's player). Identity, XInput live, until a join, the title screen or the
/// console changes it.
struct PadTable
{
    std::array<int, kPads>            lanes{0, 1, 2, 3, 4, 5, 6, 7};
    std::array<std::uint8_t, kSlots>  live{};

    constexpr bool operator==(const PadTable&) const noexcept = default;
};
inline constexpr PadTable kIdentity{};

/// A table whose XInput lanes are `xinput` (the PlayStation lanes identity, XInput live everywhere).
constexpr PadTable FromXInput(const std::array<int, kSlots>& xinput) noexcept
{
    PadTable table{};
    for (int slot = 0; slot < kSlots; ++slot)
    {
        table.lanes[static_cast<std::size_t>(slot)] = xinput[static_cast<std::size_t>(slot)];
    }
    return table;
}

/// Each lane holds one of its own reader's pads, each pad once; each slot's live reader is a reader.
constexpr bool IsValid(const PadTable& table) noexcept
{
    std::array<bool, kPads> seen{};
    for (int lane = 0; lane < kPads; ++lane)
    {
        const int pad = table.lanes[static_cast<std::size_t>(lane)];
        if (pad < 0 || pad >= kPads || ReaderOf(pad) != ReaderOf(lane) || seen[static_cast<std::size_t>(pad)])
        {
            return false;
        }
        seen[static_cast<std::size_t>(pad)] = true;
    }
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int reader = table.live[static_cast<std::size_t>(slot)];
        if (reader >= kReaders && !(slot == 0 && reader == kKeyboard))
        {
            return false;  // the keyboard plays player 1 only
        }
    }
    return true;
}

/// The slot `pad` answers, or -1 (never for a valid table).
constexpr int SlotOf(const PadTable& table, int pad) noexcept
{
    for (int lane = 0; lane < kPads; ++lane)
    {
        if (table.lanes[static_cast<std::size_t>(lane)] == pad)
        {
            return lane % kSlots;
        }
    }
    return -1;
}

/// The pad on `slot` that plays its player: the slot's live reader's; kNoPad when the keyboard plays it.
constexpr int PadOn(const PadTable& table, int slot) noexcept
{
    const int reader = table.live[static_cast<std::size_t>(slot)];
    return reader >= kReaders ? kNoPad : table.lanes[static_cast<std::size_t>(Lane(reader, slot))];
}

/// The keyboard plays player 1.
constexpr bool KeyboardPlaysOne(const PadTable& table) noexcept
{
    return table.live[0] == kKeyboard;
}

/// The table with the keyboard playing player 1: every pad on player 1's slot is muted while it does (a pad's press on
/// the title takes player 1 back, WithPadOn).
constexpr PadTable WithKeyboardPlayerOne(PadTable table) noexcept
{
    table.live[0] = static_cast<std::uint8_t>(kKeyboard);
    return table;
}

/// The table with pad `which` on `slot` (swapped, in its own reader, with the pad there) and playing its player: the
/// slot's live reader becomes the pad's.
constexpr PadTable WithPadOn(PadTable table, int which, int slot) noexcept
{
    const int from = SlotOf(table, which);
    if (from < 0 || slot < 0 || slot >= kSlots)
    {
        return table;
    }
    const int  reader = ReaderOf(which);
    const auto at     = static_cast<std::size_t>(Lane(reader, slot));
    table.lanes[static_cast<std::size_t>(Lane(reader, from))] = table.lanes[at];
    table.lanes[at]                                            = which;
    table.live[static_cast<std::size_t>(slot)]                 = static_cast<std::uint8_t>(reader);
    return table;
}

// ---------------------------------------------------------------------------------------------------------------------
// The opener: View held for kOpenHoldMs on one pad opens that player's window (View with Menu raised Steam's on-screen
// keyboard over the game). View alone opens the map at its press, so a View press is held back from the game: let go
// before kOpenHoldMs, the game gets it then as a one-poll tap (the map opens at the let-go, a tap's length late); held
// kOpenHoldMs, the opener fires once and the game never sees that press.

inline constexpr std::uint16_t kOpenButton = kView;
inline constexpr std::int64_t  kOpenHoldMs = 1000;

struct OpenHold
{
    bool         down  = false;  // View is down and held back from the game
    bool         fired = false;  // the opener fired on this press: hidden until View is up
    std::int64_t since = 0;      // when it went down
};

struct Gated
{
    std::uint16_t buttons = 0;      // what the game sees
    bool          open    = false;  // the opener fired on this poll
};

/// One poll of one physical pad: `raw` its buttons, `nowMs` a steady clock.
constexpr Gated Gate(OpenHold& h, std::uint16_t raw, std::int64_t nowMs) noexcept
{
    const bool view = (raw & kOpenButton) != 0;
    auto       out  = static_cast<std::uint16_t>(raw & ~kOpenButton);
    if (!view)
    {
        // Let go: a press held back and not fired reaches the game as a tap (pressed on this poll, up on the next).
        if (h.down && !h.fired)
        {
            out = static_cast<std::uint16_t>(out | kOpenButton);
        }
        h = OpenHold{};
        return {out, false};
    }
    if (!h.down)
    {
        h.down  = true;
        h.since = nowMs;
    }
    if (!h.fired && nowMs - h.since >= kOpenHoldMs)
    {
        h.fired = true;
        return {out, true};
    }
    return {out, false};
}

// ---------------------------------------------------------------------------------------------------------------------
// A button that is a tap or a long press, not held back from the game (a window's B: a tap closes it, a hold leaves
// the game): the hold fires once after `ms` down, again only after a let-go; a let-go before then is the tap, told at
// the let-go, so a press that grows into the hold never also taps.

struct Held
{
    bool         down  = false;
    bool         fired = false;
    std::int64_t since = 0;
};

enum class Long : std::uint8_t
{
    None,
    Tap,   // let go before `ms`
    Hold,  // down `ms`
};

constexpr Long Hold(Held& h, bool down, std::int64_t nowMs, std::int64_t ms) noexcept
{
    if (!down)
    {
        const bool tapped = h.down && !h.fired;
        h                 = Held{};
        return tapped ? Long::Tap : Long::None;
    }
    if (!h.down)
    {
        h.down  = true;
        h.since = nowMs;
    }
    if (!h.fired && nowMs - h.since >= ms)
    {
        h.fired = true;
        return Long::Hold;
    }
    return Long::None;
}

/// `slot` holds a player while `playing` (bit N: slot N holds a player; slot 0, player 1's, always).
constexpr bool PlayerSlot(int slot, std::uint32_t playing) noexcept
{
    return slot == 0 || (slot >= 1 && slot < 32 && (playing & (1u << slot)) != 0);
}

/// What the game sees of a pad on `slot` while `playing`: Start only on a player's slot, so a pad that plays no one
/// never pauses the game; its Start joins instead (session/player_window).
constexpr std::uint16_t StartGate(std::uint16_t buttons, int slot, std::uint32_t playing) noexcept
{
    return PlayerSlot(slot, playing) ? buttons : static_cast<std::uint16_t>(buttons & ~kStart);
}

/// `pad` is muted: it answers (`present`, bit p: pad p answers) on a player's slot (`playing`) whose live reader is
/// another one, while that reader's pad there answers too, or the keyboard plays that slot. It reads idle to the game and plays no one, so its Start
/// joins. A slot's other pad while the live one is absent plays on (one person, one pad, whichever kind it is).
constexpr bool Muted(const PadTable& table, std::uint32_t playing, std::uint32_t present, int pad) noexcept
{
    const int slot = SlotOf(table, pad);
    if ((present >> pad & 1u) == 0 || slot < 0 || !PlayerSlot(slot, playing) ||
        table.live[static_cast<std::size_t>(slot)] == ReaderOf(pad))
    {
        return false;
    }
    const int live = PadOn(table, slot);
    return live == kNoPad || (present >> live & 1u) != 0;  // the keyboard is always there
}

// One pad read by both readers: Steam Input shows a PlayStation pad as an XInput pad while the game also reads it
// itself, so each press comes from two pads, of different readers, in the same poll or the next.
inline constexpr std::int64_t kTwinMs = 100;

/// Two presses of one button (their polls' clock, 0 for none) are one press read twice.
constexpr bool Twins(std::int64_t at, std::int64_t otherAt) noexcept
{
    return at != 0 && otherAt != 0 && (at > otherAt ? at - otherAt : otherAt - at) <= kTwinMs;
}

/// A Start read twice: `pad`'s copy joins no one when its twin plays a player (the Start is that player's), or when it
/// is the XInput copy (one join, the PlayStation copy's: the game's own read of the pad, with its button art).
constexpr bool TwinStartDropped(int pad, bool twinPlays) noexcept
{
    return twinPlays || ReaderOf(pad) == kXInput;
}

/// The muted pads (bit p: pad p).
constexpr std::uint32_t MutedPads(const PadTable& table, std::uint32_t playing, std::uint32_t present) noexcept
{
    std::uint32_t pads = 0;
    for (int pad = 0; pad < kPads; ++pad)
    {
        pads |= Muted(table, playing, present, pad) ? 1u << pad : 0u;
    }
    return pads;
}

// ---------------------------------------------------------------------------------------------------------------------
// The let-go: after a pad left a window or changed slots, a pad still holding what it held then stays idle to the game
// until it lets go of that, so the press that closed a window never reaches the game. Only what was held at the first
// poll counts (a stuck or resting input never holds a pad for good): a button when its bit goes up, a trigger when it
// drops to the threshold or by the threshold from where it was. A trigger that still has not let go kLetGoCapMs after
// every button did (a worn trigger resting high) lets the pad go anyway, and so does a button still down
// kLetGoButtonCapMs after the change (a stuck bit; a hold that long is no press meant for the game).

inline constexpr std::uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
inline constexpr std::int64_t kLetGoCapMs       = 2000;
inline constexpr std::int64_t kLetGoButtonCapMs = 10000;

struct LetGo
{
    bool          waiting   = false;
    bool          armed     = false;  // the first poll's inputs are stored
    std::uint16_t held      = 0;      // buttons held at the first poll, not yet up
    bool          leftHeld  = false, rightHeld = false;
    std::uint8_t  leftAt    = 0, rightAt = 0;  // the triggers at the first poll
    std::int64_t  armedAt   = 0;               // the first poll
    std::int64_t  buttonsUp = -1;              // when every held button was up while a trigger still held
};

enum class Wait : std::uint8_t
{
    None,           // not waiting: the pad reaches the game
    Holding,        // still holding what it held: idle to the game
    LetGoNow,       // let go on this poll: idle once more (the last of a held-back tap is not delivered), then free
    TriggerRests,   // every button up, a trigger still high after the cap: idle once more, then free
    ButtonStuck,    // a button held at the change still down after its cap: idle once more, then free
};

constexpr void AwaitLetGo(LetGo& w) noexcept
{
    w = LetGo{.waiting = true};
}

constexpr bool TriggerLetGo(std::uint8_t now, std::uint8_t at) noexcept
{
    return now <= kTriggerThreshold || static_cast<int>(now) + kTriggerThreshold <= static_cast<int>(at);
}

/// One poll of a pad that may be waiting.
constexpr Wait Poll(LetGo& w, std::uint16_t buttons, std::uint8_t left, std::uint8_t right, std::int64_t nowMs) noexcept
{
    if (!w.waiting)
    {
        return Wait::None;
    }
    if (!w.armed)
    {
        w.armed     = true;
        w.armedAt   = nowMs;
        w.held      = buttons;
        w.leftAt    = left;
        w.rightAt   = right;
        w.leftHeld  = left > kTriggerThreshold;
        w.rightHeld = right > kTriggerThreshold;
    }
    w.held      = static_cast<std::uint16_t>(w.held & buttons);
    w.leftHeld  = w.leftHeld && !TriggerLetGo(left, w.leftAt);
    w.rightHeld = w.rightHeld && !TriggerLetGo(right, w.rightAt);
    if (w.held == 0 && !w.leftHeld && !w.rightHeld)
    {
        w = LetGo{};
        return Wait::LetGoNow;
    }
    if (w.held != 0)
    {
        w.buttonsUp = -1;
        if (nowMs - w.armedAt >= kLetGoButtonCapMs)
        {
            w = LetGo{};
            return Wait::ButtonStuck;
        }
        return Wait::Holding;
    }
    if (w.buttonsUp < 0)
    {
        w.buttonsUp = nowMs;
    }
    if (nowMs - w.buttonsUp >= kLetGoCapMs)
    {
        w = LetGo{};
        return Wait::TriggerRests;
    }
    return Wait::Holding;
}

/// B's hold: down this long on a player's pad with its window open.
inline constexpr std::int64_t kLeaveHoldMs = 1000;

}  // namespace lss::input::pad_rules
