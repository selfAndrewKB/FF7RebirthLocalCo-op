#include "hud/button_art.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/hud.h"
#include "hud/button_art_rules.h"
#include "hud/label_states.h"
#include "hud/text_blocks.h"
#include "input/pad_rules.h"
#include "input/pad_table.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::button_art {

namespace {

namespace rules = button_art_rules;
using label_states::LabelAccess;
using label_states::LabelBook;
using label_states::Parked;
using rules::Art;

using OptionDataFn = const std::uint8_t* (*)();

constexpr std::size_t kHashedChars     = 256;  // a text's first characters, hashed for its state's stamp
constexpr std::size_t kLoggedChars     = 40;
constexpr std::size_t kMaxButtonBlocks = LabelBook::kMaxLabels;  // button blocks followed through one pass
constexpr std::size_t kMaxRelaid       = 64;   // player 1's states laid out again after one pass
constexpr ULONGLONG   kCensusEveryMs = 10'000;

// Set once at install.
bool         g_installed  = false;
OptionDataFn g_optionData = nullptr;

// Game thread.
bool g_on = true;

using Fonts = std::array<void*, facts::kButtonFonts.size()>;  // the UI fonts the game writes, in its order

/// The pass open now: its seat (0 for none), the seat's art and the fonts it was written into, with the game's bytes.
struct Pass
{
    int                                         seat = 0;
    Art                                         art{};
    Fonts                                       fonts{};
    std::array<Art, facts::kButtonFonts.size()> saved{};
};
Pass g_pass;

/// A button block at the pass's start, by address: what its layout depends on outside the swapped ranges, and whether
/// the seat's state went in. At the pass's end it tells which of player 1's states the pass made stale: one whose
/// text the pass changed (the seat's updates set their own), and one laid out in the pass without a seat state (with
/// the seat's bytes in the fonts).
struct Seen
{
    void*         block   = nullptr;
    const void*   text    = nullptr;
    std::int32_t  num     = 0;
    float         wrap    = 0.0f;
    std::uint8_t  pending = 0;
    bool          swapped = false;
};
std::array<Seen, kMaxButtonBlocks> g_seen{};
std::size_t                        g_seenCount = 0;

/// Each player's art as last logged (index 0: the game's own, player 1's).
std::array<Art, players::kMaxPlayers>  g_loggedArt{};
std::array<bool, players::kMaxPlayers> g_loggedKnown{};
std::array<bool, players::kMaxPlayers> g_loggedOwn{};  // the seat's art differed from the game's

struct Counts
{
    std::uint64_t passes = 0, ownArt = 0, buttonBlocks = 0, unswapped = 0, notFollowed = 0, relaidForOne = 0,
                  staleForOne = 0;
};
std::array<Counts, players::kMaxPlayers> g_passCounts{};   // each seat's passes so far ([0] unused)
std::array<Counts, players::kMaxPlayers> g_logged{};
ULONGLONG                                g_censusAt      = 0;
bool                                     g_loggedNoFonts = false;
bool                                     g_refused       = false;  // a swap faulted: the game's art for everyone

using platform::Read;

// --- The book's engine side ---

bool Usable(void* block) noexcept
{
    return text_blocks::Unswappable(block) == nullptr;
}

/// The text's first characters and its length, hashed (FNV-1a): a seat state laid out for another text is stale.
std::uint64_t TextHash(void* block) noexcept
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    std::uint64_t  hash = 1469598103934665603ull;
    if (!Read(block, text_blocks::kText, data) || !Read(block, text_blocks::kTextNum, num) || data == nullptr || num <= 1)
    {
        return hash;
    }
    std::array<wchar_t, kHashedChars> chars{};
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num - 1), kHashedChars);
    if (!platform::CopyGuarded(chars.data(), data, count * sizeof(wchar_t)))
    {
        return hash;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        hash = (hash ^ static_cast<std::uint64_t>(chars[i])) * 1099511628211ull;
    }
    return hash ^ static_cast<std::uint64_t>(num);
}

/// What a seat's state depends on outside its ranges: the block's fonts and language, its text, and the seat's art.
std::uint64_t Stamp(void* block) noexcept
{
    const std::uint64_t art = (static_cast<std::uint64_t>(g_pass.art.family) << 8) | g_pass.art.keyboard;
    return text_blocks::Stamp(block) ^ std::rotl(TextHash(block), 7) ^ std::rotl(art, 56);
}

/// A new seat state: empty, its relayout pending, which the block's own Tick runs when the seat's pass paints it.
Parked FreshState() noexcept
{
    Parked fresh   = label_states::EmptyState();
    fresh.flags[0] = std::byte{1};  // kPending, the first of the flags
    return fresh;
}
static_assert(text_blocks::kPending == label_states::kFlagsOffset, "the fresh state's pending byte is the first flag");

constexpr LabelAccess kAccess{&Usable,
                              &text_blocks::Alive,
                              &text_blocks::SwapLayout,
                              &text_blocks::Invalidate,
                              &Stamp,
                              &text_blocks::HoldWeak,
                              &text_blocks::ReleaseWeak,
                              &text_blocks::FreeParked};
static_assert(players::kMaxPlayers == 3, "one book per player, listed below");
std::array<LabelBook, players::kMaxPlayers> g_books{LabelBook(kAccess, FreshState()), LabelBook(kAccess, FreshState()),
                                                    LabelBook(kAccess, FreshState())};

LabelBook& BookOf(int seat) noexcept
{
    return g_books[static_cast<std::size_t>(seat)];
}

Counts& CountsOf(int seat) noexcept
{
    return g_passCounts[static_cast<std::size_t>(seat)];
}

// --- Each player's art ---

/// The three UI fonts the game writes (null where one is not loaded); false before the fonts load.
bool ButtonFonts(Fonts& fonts) noexcept
{
    const void*   holder = players::game_globals::UiSlot(facts::kUIFontHolder / sizeof(void*));
    const void*   data   = nullptr;
    std::int32_t  num    = 0;
    if (holder == nullptr || !Read(holder, 0, data) || !Read(holder, facts::kFontHolderNum, num) || data == nullptr)
    {
        return false;
    }
    for (std::size_t i = 0; i < fonts.size(); ++i)
    {
        const std::uint32_t index = facts::kButtonFonts[i];
        fonts[i] = static_cast<std::int32_t>(index) < num
                       ? players::weak_object::ObjectAt(static_cast<const std::byte*>(data) + sizeof(std::uint64_t) * index)
                       : nullptr;
    }
    return fonts[0] != nullptr;
}

bool ReadArt(const void* font, Art& art) noexcept
{
    return font != nullptr && Read(font, facts::kFontPadFamily, art.family) && Read(font, facts::kFontKeyboard, art.keyboard);
}

void WriteArt(void* font, const Art& art) noexcept
{
    if (font != nullptr)
    {
        platform::WriteGuarded(static_cast<std::byte*>(font) + facts::kFontPadFamily, &art.family, sizeof(art.family));
        platform::WriteGuarded(static_cast<std::byte*>(font) + facts::kFontKeyboard, &art.keyboard, sizeof(art.keyboard));
    }
}

/// The pad on `seat`'s slot by the slot's live reader, or pad_rules::kNoPad when the keyboard plays it.
int PlayingPad(int seat) noexcept
{
    return input::pad_rules::PadOn(input::pad_table::Table(), seat);
}

/// Steam's pad type for the XInput pad that plays `seat` (0 when unknown or not an XInput pad). The game fills the types
/// by Steam's pad index, which follows the physical pad, not the slot the pad table moved it to [ASSUMPTION: the index
/// order is XInput's].
std::uint8_t PadType(int seat) noexcept
{
    const int           pad   = PlayingPad(seat);
    const void*         owner = players::game_globals::Object(players::game_globals::Global::SquareInputOwner);
    const void*         types = nullptr;
    const std::uint8_t* data  = nullptr;
    std::int32_t        num   = 0;
    std::uint8_t        type  = 0;
    if (pad < 0 || input::pad_rules::ReaderOf(pad) != input::pad_rules::kXInput || owner == nullptr ||
        !Read(owner, facts::kInputPadTypes, types) || types == nullptr || !Read(types, facts::kPadTypesData, data) ||
        !Read(types, facts::kPadTypesNum, num) || data == nullptr || pad >= num || !Read(data + pad, type))
    {
        return 0;
    }
    return type;
}

std::uint8_t ButtonOption() noexcept
{
    const std::uint8_t* options = nullptr;
    std::uint8_t        option  = rules::kOptionAuto;
    if (g_optionData != nullptr && platform::CallGuarded(g_optionData, options) && options != nullptr)
    {
        Read(options + facts::kOptionButtonDisplay, option);
    }
    return option;
}

/// The device that plays `seat` (no pad: the keyboard).
rules::Device DeviceOf(int seat) noexcept
{
    const int pad = PlayingPad(seat);
    return pad < 0                                                       ? rules::Device::Keyboard
           : input::pad_rules::ReaderOf(pad) == input::pad_rules::kXInput ? rules::Device::XInputPad
                                                                          : rules::Device::PlayStationPad;
}
const wchar_t* ArtName(const Art& art) noexcept
{
    if (art.keyboard != 0)
    {
        return L"keycaps";
    }
    switch (art.family)
    {
    case rules::kFamilyPlayStation:
        return L"PlayStation art (DualSense)";
    case rules::kFamilyXbox:
        return L"Xbox art";
    case rules::kFamilyDualShock4:
        return L"PlayStation art (DualShock 4)";
    case rules::kFamilyFourth:
        return L"the fourth art set";
    default:
        return L"DirectInput art";
    }
}

/// An Info line when a player's art changes (player 1's: the game's own).
void NoteArt(int seat, const Art& art, bool own) noexcept
{
    const auto s = static_cast<std::size_t>(seat);
    if (g_loggedKnown[s] && g_loggedArt[s] == art && g_loggedOwn[s] == own)
    {
        return;
    }
    g_loggedKnown[s] = true;
    g_loggedArt[s]   = art;
    g_loggedOwn[s]   = own;
    if (seat == 0)
    {
        platform::LogDebug(L"button art: the game's own (player 1's view, and every view while one shows): {}", ArtName(art));
        return;
    }
    platform::Log(L"button art: {}'s view shows {}{}", players::SeatName(seat), ArtName(art),
                  own ? L" (its own state of each button text)" : L", as the game's own");
}

// --- The passes ---

bool IsButtonBlock(void* block) noexcept
{
    std::uint8_t button = 0;
    return Read(block, text_blocks::kButtonText, button) && button != 0;
}

Seen SeenOf(void* block) noexcept
{
    Seen s;
    s.block = block;
    Read(block, text_blocks::kText, s.text);
    Read(block, text_blocks::kTextNum, s.num);
    Read(block, text_blocks::kWrapWidth, s.wrap);
    Read(block, text_blocks::kPending, s.pending);
    return s;
}

bool SameText(const Seen& a, const Seen& b) noexcept
{
    return a.text == b.text && a.num == b.num;
}

/// Nothing the layout reads changed and no relayout was pending: the pass did not lay the block out.
bool SameLayoutInputs(const Seen& a, const Seen& b) noexcept
{
    return SameText(a, b) && std::bit_cast<std::uint32_t>(a.wrap) == std::bit_cast<std::uint32_t>(b.wrap) &&
           a.pending == 0 && b.pending == 0;
}

const Seen* FindSeen(const void* block) noexcept
{
    const Seen* const begin = g_seen.data();
    const Seen* const end   = begin + g_seenCount;
    const Seen* const at    = std::lower_bound(begin, end, block,
                                             [](const Seen& s, const void* b) { return std::less<>{}(s.block, b); });
    return at != end && at->block == block ? at : nullptr;
}

/// The game's bytes back in the fonts the pass wrote (any thread: the fonts are resident assets).
void RestoreFonts() noexcept
{
    for (std::size_t i = 0; i < g_pass.fonts.size(); ++i)
    {
        WriteArt(g_pass.fonts[i], g_pass.saved[i]);
    }
}

/// A new button block's census line: its text and whether its font is one the game writes.
void LogFirstSeen(int seat, void* block) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    void*          font = nullptr;
    Read(block, text_blocks::kText, data);
    Read(block, text_blocks::kTextNum, num);
    Read(block, text_blocks::kFont, font);
    std::array<wchar_t, kLoggedChars> chars{};
    const std::size_t count = data != nullptr && num > 1 ? std::min<std::size_t>(static_cast<std::size_t>(num - 1), kLoggedChars) : 0;
    if (count != 0 && !platform::CopyGuarded(chars.data(), data, count * sizeof(wchar_t)))
    {
        return;
    }
    std::wstring text(chars.data(), count);
    std::replace_if(text.begin(), text.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }, L' ');
    const bool written = std::find(g_pass.fonts.begin(), g_pass.fonts.end(), font) != g_pass.fonts.end();
    Art        art{};
    ReadArt(font, art);
    platform::LogDebug(L"button art: {}'s first state of {} '{}' ({}; its font {})", players::SeatName(seat), block,
                       text, num > static_cast<std::int32_t>(kLoggedChars) + 1 ? L"cut" : L"whole",
                       written ? std::wstring(L"one of the three the game writes")
                               : std::format(L"another one, its bytes {}/{}: the game never writes them", art.family,
                                             art.keyboard));
}

void Census() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_censusAt)
    {
        return;
    }
    g_censusAt = now + kCensusEveryMs;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto     s = static_cast<std::size_t>(seat);
        const Counts&  c = g_passCounts[s];
        Counts&        l = g_logged[s];
        if (c.passes == l.passes)
        {
            continue;
        }
        const label_states::LabelCounts& b = g_books[s].Counts();
        platform::LogDebug(L"button art (10 s), {}: passes {} (with its own art {}), button blocks per pass {:.1f}; "
                           L"states {} (so far: new {}, laid out again {}, refused {}, book full {}, faults {}); kept player 1's "
                           L"state {} (not followed {}); player 1's laid out again after a pass {}, marked stale (its "
                           L"text changed in the pass) {}",
                           players::SeatName(seat), c.passes - l.passes, c.ownArt - l.ownArt,
                           c.ownArt != l.ownArt
                               ? static_cast<double>(c.buttonBlocks - l.buttonBlocks) / static_cast<double>(c.ownArt - l.ownArt)
                               : 0.0,
                           g_books[s].Count(), b.created, b.fontResets, b.refused, b.full, b.faults,
                           c.unswapped - l.unswapped, c.notFollowed - l.notFollowed, c.relaidForOne - l.relaidForOne,
                           c.staleForOne - l.staleForOne);
        l = c;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto options = platform::FindFunction(image, facts::kSystemOptionData);
    g_installed        = options && text_blocks::Install(image) && players::weak_object::Known() &&
                  players::game_globals::Address(players::game_globals::Global::UiSystem) != 0 &&
                  players::game_globals::Address(players::game_globals::Global::SquareInputOwner) != 0;
    if (!g_installed)
    {
        platform::LogWarning(L"button art: the text blocks or the fonts are not what the research read: every view shows "
                             L"the game's own button art");
        return false;
    }
    g_optionData = reinterpret_cast<OptionDataFn>(*options);
    platform::Log(L"button art: each player's view shows its own device's button art (keycaps, Xbox or PlayStation); "
                  L"while one view shows, the game's own");
    return true;
}

void Remove() noexcept
{
    if (g_pass.seat != 0)
    {
        LeavePass();
    }
    if (g_pass.seat != 0)
    {
        RestoreFonts();  // off the game thread the states stay (the widgets are the game thread's), the fonts not
        g_pass.seat = 0;
    }
    if (players::generation::OnGameThread())
    {
        for (LabelBook& book : g_books)
        {
            book.ReleaseAll();
        }
    }
    g_installed = false;
}

void EnterPass(int seat) noexcept
{
    if (!g_installed || !g_on || g_refused || g_pass.seat != 0 || !players::SeatIndex(seat) || seat == 0 ||
        !players::generation::OnGameThread())
    {
        return;
    }
    Counts& counts = CountsOf(seat);
    ++counts.passes;
    Fonts fonts{};
    Art                                           shared{};
    if (!ButtonFonts(fonts) || !ReadArt(fonts[0], shared))
    {
        if (!g_loggedNoFonts)
        {
            g_loggedNoFonts = true;
            platform::Log(L"button art: the game's fonts are not loaded yet; the views show the game's art meanwhile");
        }
        return;
    }
    const Art art = rules::ArtOf(DeviceOf(seat), PadType(seat), ButtonOption(), shared);
    NoteArt(0, shared, false);
    NoteArt(seat, art, art != shared);
    if (art == shared)
    {
        return;
    }
    ++counts.ownArt;
    g_pass.seat  = seat;
    g_pass.art   = art;
    g_pass.fonts = fonts;
    for (std::size_t i = 0; i < fonts.size(); ++i)
    {
        g_pass.saved[i] = shared;
        ReadArt(fonts[i], g_pass.saved[i]);
        WriteArt(fonts[i], art);
    }
    // Collected first, then swapped: the swaps call the game's Invalidate, never inside the walk of the game's set.
    g_seenCount = 0;
    try
    {
        text_blocks::ForEachLive([&](void* block) {
            if (IsButtonBlock(block))
            {
                ++counts.buttonBlocks;
                if (g_seenCount < g_seen.size())
                {
                    g_seen[g_seenCount++] = SeenOf(block);
                }
                else
                {
                    ++counts.notFollowed;
                }
            }
            return true;
        });
    }
    catch (...)
    {
    }
    std::sort(g_seen.begin(), g_seen.begin() + static_cast<std::ptrdiff_t>(g_seenCount),
              [](const Seen& a, const Seen& b) { return std::less<>{}(a.block, b.block); });
    LabelBook& book = BookOf(seat);
    for (std::size_t i = 0; i < g_seenCount; ++i)
    {
        Seen&               seen       = g_seen[i];
        void* const         controller = text_blocks::ControllerOf(seen.block);
        const std::uint64_t created    = book.Counts().created;
        if (!book.SwapIn(seen.block, controller))
        {
            g_refused = true;
            break;
        }
        seen.swapped = book.Holds(controller);
        if (!seen.swapped)
        {
            ++counts.unswapped;
        }
        else if (book.Counts().created != created)
        {
            LogFirstSeen(seat, seen.block);
        }
    }
    if (g_refused)
    {
        platform::Log(L"button art: a text state swap raised an exception ({}); every view shows the game's art from now",
                      platform::DescribeFault(platform::LastFault()).text);
        LeavePass();
    }
}

void LeavePass() noexcept
{
    const int seat = g_pass.seat;
    if (seat == 0 || !players::generation::OnGameThread())
    {
        return;
    }
    RestoreFonts();
    LabelBook& book = BookOf(seat);
    // Found while the seat's states are still in: player 1's states whose text the pass changed (stale: laid out again
    // when player 1's pass paints them, should player 1's updates set that same text, which the game's compare skips),
    // and the button blocks the pass laid out with the seat's bytes but no state of the seat's (laid out again now).
    std::array<void*, kMaxRelaid> stale{}, relaid{};
    std::size_t                   staleCount = 0, relaidCount = 0;
    for (std::size_t i = 0; i < g_seenCount && staleCount < stale.size(); ++i)
    {
        if (g_seen[i].swapped && !SameText(g_seen[i], SeenOf(g_seen[i].block)))
        {
            stale[staleCount++] = g_seen[i].block;  // still readable: the book holds a weak reference
        }
    }
    try
    {
        text_blocks::ForEachLive([&](void* block) {
            if (!IsButtonBlock(block) || book.Holds(text_blocks::ControllerOf(block)))
            {
                return true;
            }
            const Seen* const seen = FindSeen(block);
            if (seen == nullptr || !SameLayoutInputs(*seen, SeenOf(block)))
            {
                relaid[relaidCount++] = block;
            }
            return relaidCount < relaid.size();
        });
    }
    catch (...)
    {
    }
    const std::uint64_t faults = book.Counts().faults;
    // The stamps are taken first, while the seat's states (and the pass's text) are in; the art is still the seat's.
    book.SwapBack();
    g_pass.seat = 0;
    if (book.Counts().faults != faults)
    {
        g_refused = true;  // a block kept the seat's state: no more swaps
        platform::Log(L"button art: putting player 1's text state back raised an exception; every view shows the game's art "
                      L"from now");
    }
    for (std::size_t i = 0; i < relaidCount; ++i)
    {
        text_blocks::Relayout(relaid[i]);
    }
    const std::uint8_t pending = 1;
    for (std::size_t i = 0; i < staleCount; ++i)
    {
        if (text_blocks::Alive(text_blocks::ControllerOf(stale[i])) &&
            platform::WriteGuarded(static_cast<std::byte*>(stale[i]) + text_blocks::kPending, &pending, sizeof(pending)))
        {
            text_blocks::Invalidate(stale[i]);
        }
    }
    Counts& counts = CountsOf(seat);
    counts.relaidForOne += relaidCount;
    counts.staleForOne += staleCount;
    g_seenCount = 0;
}

void Tick() noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        LabelBook& book = BookOf(seat);
        if (g_on && !g_refused && players::generation::Active(seat))
        {
            book.Sweep();
        }
        else if (book.Count() != 0)
        {
            book.ReleaseAll();
            g_loggedKnown[static_cast<std::size_t>(seat)] = false;
        }
    }
    Census();
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"button-art",
                       .shape = control::Shape::OnOff,
                       .help  = L"players 2 and 3's views show their own device's button art (off: the game's own in every "
                                L"view)",
                       .set   = [](const Value& v) {
                           if (!g_installed)
                           {
                               return Outcome{false, L"the button art is not installed (see the startup log)"};
                           }
                           g_on = std::get<bool>(v);
                           g_refused = g_refused && !g_on;  // on again: a past fault is tried afresh
                           return Outcome{true, g_on ? L"each view's own button art" : L"the game's button art everywhere"};
                       },
                       .get   = [] { return Value{g_on}; }});
    control::Register({.name  = L"players.button-art",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) each player's button art as last logged, and the states each seat keeps",
                       .get   = [] {
                           try
                           {
                               std::wstring text = g_installed ? (g_on ? L"on" : L"off") : L"not installed";
                               for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
                               {
                                   const auto s = static_cast<std::size_t>(seat);
                                   text += std::format(L"; {}: {}{}", players::SeatName(seat),
                                                       g_loggedKnown[s] ? ArtName(g_loggedArt[s]) : L"not seen",
                                                       seat != 0 ? std::format(L" ({} states)", g_books[s].Count())
                                                                 : std::wstring());
                               }
                               return Value{text};
                           }
                           catch (...)
                           {
                               return Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}

#endif

}  // namespace lss::hud::button_art
