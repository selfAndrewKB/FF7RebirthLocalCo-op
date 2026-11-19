#include "session/player_window.h"

#include "facts/controller.h"
#include "hud/save_notice.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "input/pad_presence.h"
#include "input/pad_rules.h"
#include "input/pad_table.h"
#include "modules.h"
#include "platform/guarded_read.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "session/player_window_rules.h"
#include "session/player_window_text.h"
#include "session/session.h"
#include "views/column_layout.h"
#include "views/whole_screen.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::session::player_window {

namespace {

namespace pad   = input::pad_rules;
namespace rules = player_window_rules;
namespace text  = player_window_text;

constexpr int kSeats = rules::kSeats;
static_assert(kSeats == players::kMaxPlayers, "a window per player");

struct Event
{
    int           user    = 0;
    std::uint16_t pressed = 0;
    bool          open    = false;  // View held a second
    bool          bTap    = false;  // B let go within a second
    bool          bHeld   = false;  // B held a second
};

/// A join a pad asked for, made by RunRequests before the session's tick.
struct JoinAsk
{
    int seat = -1;
    int user = -1;  // the joining pad
};

/// A line a window shows for a while: a join that waits, a refusal.
struct SeatNote
{
    text::Note   note  = text::Note::None;
    int          about = 0;
    std::int64_t until = 0;
    std::wstring aboutName;  // the character the note names, when it names one
};

constexpr std::int64_t kNoteMs              = 3000;
constexpr std::int64_t kWaitNoteMs          = 6000;  // a join's wait: longer, and said again at each Start of its pad
constexpr int          kTitleClaimedSeconds = 4;  // the notice after a press on the title: which controller plays player 1

// Noted by the pad table's listener during the engine's pad poll, taken in Tick. Game thread both.
constexpr std::size_t     kQueue = 32;
std::array<Event, kQueue> g_events{};
std::size_t               g_queued  = 0;
std::uint32_t             g_dropped = 0;

// Game thread.
bool                          g_installed = false;
Session*                      g_session   = nullptr;
std::array<bool, kSeats>      g_open{};              // by seat: the player's window is open
std::array<SeatNote, kSeats>  g_notes{};             // by seat
std::array<bool, pad::kPads>  g_bSeen{};             // by pad: B went down while its window was open (arms B)
std::array<JoinAsk, kSeats>   g_joins{};             // asked, not yet made
std::array<int, kSeats>       g_leaves{-1, -1, -1};  // players asked to leave, not yet out
std::array<bool, kSeats>      g_waiting{};           // by seat: a join that waits; its window opens once in
std::array<bool, kSeats>      g_joinPending{};       // by seat: a pad's join whose player has played no member yet
std::array<bool, kSeats>      g_waitSaid{};          // by seat: that join's wait said on player 1's window
int                           g_startWhenField = -1;  // a spare pad's Start during a menu or a scene: joins after
bool                          g_redraw         = false;
std::uint32_t                 g_playing        = 1;  // the slots a player holds, as of the last tick
std::uint32_t                 g_idlePads       = 0;  // the pads read idle now (the open windows' pads)
int                           g_lastScreens    = 0;  // the views as last shown
unsigned long long            g_gameClosedSeen = 0;  // view_window's count of windows the game closed
std::uint32_t                 g_opens = 0, g_joinsAsked = 0, g_leavesAsked = 0, g_titlePresses = 0, g_trades = 0,
                              g_takenBack = 0;
constexpr int                 kNoClaim           = -2;
int                           g_titleClaimedUser = kNoClaim;  // the pad the title's notice last named (kNoPad: the keyboard)
bool                          g_keyboardLast     = false;  // the game's last device was the keyboard, at the last good read
bool                          g_titleLast        = false;  // the title screen showed, at that read
std::array<const void*, kSeats> g_drawnCharacters{};  // by seat: the character its window names (another redraws it)

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Note(int user, std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept
{
    if (g_queued < kQueue)
    {
        g_events[g_queued++] = {user, pressed, open, bTap, bHeld};
    }
    else
    {
        ++g_dropped;
    }
}

/// The seat's character now (player 1's pawn, a seat's member), or null.
const void* CharacterOf(int seat) noexcept
{
    return seat == 0 ? players::actor_fields::Pawn(players::generation::PlayerOneController())
                     : players::generation::MemberOf(seat);
}

/// The seat's character in capitals ("TIFA"), from its member's name, else "PLAYER n".
std::wstring NameOf(int seat)
{
    const std::wstring name = text::CharacterName(players::NameOf(CharacterOf(seat)));
    return name.empty() ? text::PlayerName(seat) : name;
}

bool Plays(int seat) noexcept
{
    return rules::Plays(g_playing, seat);
}

/// Another player is in, and both play on a pad (the keyboard's player 1 trades with no one): the d-pad's left/right
/// trade numbers.
bool CanTrade(int seat) noexcept
{
    const auto table = input::pad_table::Table();
    for (const int dir : {-1, +1})
    {
        if (const int other = rules::TradePartner(seat, dir, g_playing); other >= 0 && rules::CanTradePads(table, seat, other))
        {
            return true;
        }
    }
    return false;
}

/// A seat no player holds, waits for, rejoins or is joining now (the lowest), or -1.
int FreeSeat()
{
    for (int seat = 1; g_session != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        const bool asked = std::any_of(g_joins.begin(), g_joins.end(), [seat](const JoinAsk& j) { return j.seat == seat; });
        if (!asked && !modules::SeatTaken(*g_session, seat))
        {
            return seat;
        }
    }
    return -1;
}

void SetNote(int seat, text::Note note, int about)
{
    g_notes[static_cast<std::size_t>(seat)] = {note, about, NowMs() + kNoteMs, {}};
    g_redraw                                = true;
}

/// `seat`'s player is in without a member, or out until one is free.
bool WaitsForMember(int seat) noexcept
{
    return g_session != nullptr && (g_session->WaitsForMember(seat) || g_session->AwaitingMember(seat));
}

/// Player 1's window says for a while that `seat`'s join waits: naming the character when the game's AI has it.
void SetWaitNote(int seat)
{
    const auto slot = g_session->SlotNow(seat);
    if (slot.wait == party_member::SlotWait::Unread)
    {
        return;  // the party read faulted (a reload under it): said once a reason is read
    }
    const std::wstring name = OnGameAi(slot.wait) ? text::CharacterName(players::NameOf(slot.character)) : std::wstring();
    g_notes[0] = {name.empty() ? text::Note::Waits : text::Note::WaitsOnAi, seat, NowMs() + kWaitNoteMs, name};
    g_redraw   = true;
    g_waitSaid[static_cast<std::size_t>(seat)] = true;
    platform::Log(L"player window: player 1's window says {}'s join waits{}", players::SeatName(seat),
                  name.empty() ? L"" : std::format(L" for {}, with the game's AI", name));
}

bool AnyRequest()
{
    return std::any_of(g_joins.begin(), g_joins.end(), [](const JoinAsk& j) { return j.seat >= 0; }) ||
           std::any_of(g_leaves.begin(), g_leaves.end(), [](int seat) { return seat >= 0; });
}

void Open(int seat, const std::wstring& why)
{
    g_open[static_cast<std::size_t>(seat)] = true;
    g_redraw                               = true;
    ++g_opens;
    platform::Log(L"player window: {}'s opens ({})", players::SeatName(seat), why);
}

void Close(int seat, const wchar_t* why)
{
    if (!g_open[static_cast<std::size_t>(seat)])
    {
        return;
    }
    g_open[static_cast<std::size_t>(seat)] = false;
    g_redraw                               = true;
    platform::Log(L"player window: {}'s closes ({})", players::SeatName(seat), why);
}

/// Every window and note in each player's own view (the game's message window, hud/view_window; else the engine-font
/// prompt, hud/view_prompt); the rest closed.
void ShowWindows()
{
    hud::view_prompt::ClearAll();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const SeatNote& note = g_notes[static_cast<std::size_t>(seat)];
        if (!Plays(seat) || (!g_open[static_cast<std::size_t>(seat)] && note.note == text::Note::None))
        {
            hud::view_window::Close(seat);
            continue;
        }
        if (CharacterOf(seat) == nullptr)
        {
            // A player just in, before their member is taken: shown once named (the character's arrival redraws), so
            // the game's window does not open and reopen for the name.
            g_drawnCharacters[static_cast<std::size_t>(seat)] = nullptr;
            hud::view_window::Close(seat);
            continue;
        }
        text::Window w;
        w.seat       = seat;
        w.name       = NameOf(seat);
        g_drawnCharacters[static_cast<std::size_t>(seat)] = CharacterOf(seat);
        w.open       = g_open[static_cast<std::size_t>(seat)];
        w.canTrade   = CanTrade(seat);
        w.canMove    = g_lastScreens >= 2;
        w.startJoins = seat == 0 && FreeSeat() >= 0;
        w.note       = note.note;
        w.about      = note.about;
        w.aboutName  = note.aboutName;
        const text::ScreenText t = text::TextFor(w);
        if (!hud::view_window::Show(seat, t.markup))
        {
            hud::view_prompt::Prompt p;
            p.lines     = t.plain;
            p.highlight = 0;
            hud::view_prompt::Show(seat, p);
        }
    }
}

/// The pads of the open windows read idle to the game; the others play on.
void SetIdlePads()
{
    std::uint32_t pads  = 0;
    const auto    table = input::pad_table::Table();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        if (const int on = pad::PadOn(table, seat); g_open[static_cast<std::size_t>(seat)] && on >= 0)
        {
            pads |= 1u << on;
        }
    }
    if (pads != g_idlePads)
    {
        g_idlePads = pads;
        input::pad_table::SetIdlePads(pads, L"the open player windows");
    }
}

/// The seat pad `which` plays, or -1 (player_window_rules::SeatOf, with the pads answering now).
int SeatOfPad(int which)
{
    return rules::SeatOf(input::pad_table::Table(), g_playing, input::pad_table::PresentPads(), which);
}

/// A spare pad's Start: takes back the lowest player whose pad is gone (a pad that died and came back on another
/// number), else a join on the lowest seat no player holds (during a menu or a scene, once the field shows).
void SpareStart(int user)
{
    if (std::any_of(g_joins.begin(), g_joins.end(), [user](const JoinAsk& j) { return j.user == user; }))
    {
        return;  // asked already: its join is made before the session's next tick
    }
    const auto table = input::pad_table::Table();
    // One pad read by both readers (Steam Input's XInput copy of a PlayStation pad the game also reads) presses Start
    // on both at once: that Start is the player's whose pad the other copy is, else one join, the PlayStation copy's.
    if (const int twin = input::pad_table::TwinOf(user, pad::kStart);
        twin >= 0 && pad::TwinStartDropped(user, SeatOfPad(twin) >= 0))
    {
        const int seat = SeatOfPad(twin);
        platform::Log(L"player window: {} pressed Start at the same moment as {}: one pad read twice (Steam Input); {}",
                      pad::PadName(user), pad::PadName(twin),
                      seat >= 0 ? std::format(L"the Start is {}'s, no join", players::SeatName(seat))
                                : std::format(L"only {} joins", pad::PadName(twin)));
        return;
    }
    if (const int slot = pad::SlotOf(table, user);
        slot >= 1 && slot < kSeats && pad::PadOn(table, slot) == user && g_session != nullptr &&
        modules::SeatTaken(*g_session, slot))
    {
        platform::Log(L"player window: {} pressed Start, but its player is already joining", pad::PadName(user));
        // Its seat waits for a member: a second Start never takes another seat, and says the wait again in the field.
        if (WaitsForMember(slot) && !views::WholeScreenShown())
        {
            SetWaitNote(slot);
        }
        return;
    }
    if (const int seat = rules::SeatWithPadGone(table, g_playing, input::pad_presence::GonePads()); seat >= 0)
    {
        if (AnyRequest())
        {
            // A join or leave on its way names a number from the players as they were; the take-back waits for it.
            platform::Log(L"player window: {} pressed Start to take back {}, whose pad is gone, but a join or leave is "
                          L"on its way; press again once it is done",
                          pad::PadName(user), players::SeatName(seat));
            return;
        }
        if (input::pad_table::Set(rules::WithPadOn(table, user, seat), L"a spare pad takes back a player whose pad is gone"))
        {
            g_bSeen[static_cast<std::size_t>(user)] = false;
            g_redraw                                = true;
            ++g_takenBack;
            platform::Log(L"player window: {} pressed Start: it takes back {}, whose {} is gone", pad::PadName(user),
                          players::SeatName(seat), pad::PadName(pad::PadOn(table, seat)));
        }
        return;
    }
    const int free = FreeSeat();
    if (free < 0)
    {
        SetNote(0, text::Note::AllIn, 0);
        platform::Log(L"player window: {} pressed Start, but every player is in or on the way", pad::PadName(user));
        return;
    }
    if (views::WholeScreenShown())
    {
        g_startWhenField = user;
        platform::Log(L"player window: {} pressed Start during a menu or a scene; it joins once the field shows",
                      pad::PadName(user));
        return;
    }
    for (JoinAsk& j : g_joins)
    {
        if (j.seat < 0)
        {
            j = {free, user};
            break;
        }
    }
    ++g_joinsAsked;
    platform::Log(L"player window: {} pressed Start: it joins as {}", pad::PadName(user), players::SeatName(free));
}

/// The session's answer to a join: in (its window opens), or waiting for a member (said on player 1's window).
void AnswerJoin(const JoinAsk& j)
{
    g_redraw                                  = true;
    g_bSeen[static_cast<std::size_t>(j.user)] = false;  // a B from before the join never closes or leaves
    g_joinPending[static_cast<std::size_t>(j.seat)] = true;  // its wait, if any, is said once the session finds it
    g_waitSaid[static_cast<std::size_t>(j.seat)]    = false;
    if (g_session->In(j.seat))
    {
        g_open[static_cast<std::size_t>(j.seat)] = true;
        platform::Log(L"player window: {} is in on {}; its window opens", players::SeatName(j.seat), pad::PadName(j.user));
        return;
    }
    if (g_session->AwaitingMember(j.seat))
    {
        g_waiting[static_cast<std::size_t>(j.seat)] = true;
        SetWaitNote(j.seat);
        platform::Log(L"player window: {} joins once a party member is free", players::SeatName(j.seat));
        return;
    }
    SetNote(0, text::Note::Failed, j.seat);
    platform::Log(L"player window: {}'s join failed (see the session's lines above)", players::SeatName(j.seat));
}

/// A pad's join until its player plays a member: the wait said once on player 1's window when the session finds the
/// slot's member not free (in the field), and that note taken down once the member is played or the join is over.
void PendingJoin(int seat, const Frame& frame)
{
    const auto at = static_cast<std::size_t>(seat);
    if (!g_joinPending[at] || g_session == nullptr)
    {
        return;
    }
    const bool over = g_session->PlaysMember(seat) ||
                      (!g_session->In(seat) && !g_session->AwaitingMember(seat) && !g_session->RejoinPending(seat));
    if (over)
    {
        g_joinPending[at] = false;
        g_waitSaid[at]    = false;
        SeatNote& one     = g_notes[0];
        if (one.about == seat && (one.note == text::Note::Waits || one.note == text::Note::WaitsOnAi))
        {
            one      = {};
            g_redraw = true;
        }
        return;
    }
    if (!g_waitSaid[at] && WaitsForMember(seat) && !frame.single && !views::WholeScreenShown())
    {
        SetWaitNote(seat);
    }
}

/// B held on `seat`'s pad with its window open: player 2 or 3 leaves; player 1 is refused.
void HoldLeave(int seat, int user)
{
    if (seat == 0)
    {
        SetNote(0, text::Note::PlayerOneStays, 0);
        platform::Log(L"player window: {} held B, but player 1 stays in", pad::PadName(user));
        return;
    }
    if (std::find(g_leaves.begin(), g_leaves.end(), seat) == g_leaves.end())
    {
        for (int& s : g_leaves)
        {
            if (s < 0)
            {
                s = seat;
                break;
            }
        }
        ++g_leavesAsked;
        platform::Log(L"player window: {} held B: {} leaves", pad::PadName(user), players::SeatName(seat));
    }
}

/// On the title screen: the pad that pressed becomes player 1's, said on the game's save-data notice (the title's one
/// notice) for a few seconds, once per pad that takes the place (its own later presses, the title menu's, say
/// nothing more; another pad's press says that pad).
void TitlePress(int user, std::uint16_t pressed)
{
    // One pad read by both readers: its PlayStation copy takes player 1 (the game's own read of it: its button art,
    // trigger effects and light bar), its XInput copy stays muted beside it.
    if (pad::ReaderOf(user) == pad::kXInput && pressed != 0 &&
        input::pad_table::TwinOf(user, static_cast<std::uint16_t>(pressed & (~pressed + 1u))) >= 0)
    {
        return;
    }
    const auto table = input::pad_table::Table();
    if (pad::PadOn(table, 0) != user)
    {
        ++g_titlePresses;
        input::pad_table::Set(rules::WithPlayerOne(table, user), L"a pad pressed on the title screen: it plays player 1");
    }
    if (user != g_titleClaimedUser)
    {
        g_titleClaimedUser = user;
        hud::save_notice::Show(text::TitleClaimed(user), kTitleClaimedSeconds);
    }
}

/// On the title screen: a key (or a mouse button) pressed makes the keyboard player 1's, as a pad's press makes that
/// pad's; every pad then plays no one until its Start adds a player. Said on the same notice.
void KeyboardTitlePress()
{
    const auto table = input::pad_table::Table();
    if (!pad::KeyboardPlaysOne(table))
    {
        ++g_titlePresses;
        input::pad_table::Set(pad::WithKeyboardPlayerOne(table),
                              L"a key pressed on the title screen: the keyboard plays player 1, a pad's Start adds a player");
    }
    if (g_titleClaimedUser != pad::kNoPad)
    {
        g_titleClaimedUser = pad::kNoPad;
        hud::save_notice::Show(text::TitleClaimed(pad::kNoPad), kTitleClaimedSeconds);
    }
}

/// The game's device class turning to the keyboard's on the title screen: the keyboard's press there (the game's own
/// "keyboard or mouse button used" flag, folded into the class by its input tick; a mouse move does not count).
void WatchKeyboardOnTitle()
{
    std::uint8_t deviceClass = 0;
    if (!platform::Read(players::game_globals::Object(players::game_globals::Global::SquareInputOwner),
                        facts::kInputDeviceClass, deviceClass))
    {
        return;  // unread: no edge from a failed read
    }
    const bool keyboard = deviceClass == facts::kDeviceClassKeyboard;
    const bool title    = views::AtTitleNow();
    if (rules::KeyboardClaims(keyboard, g_keyboardLast, title, g_titleLast))
    {
        KeyboardTitlePress();
    }
    g_keyboardLast = keyboard;
    g_titleLast    = title;
}

/// Each screen's seat now (-1 for a copy's): the engine's list placed by the screen order, as the layout places it.
std::array<int, kSeats> SeatOnScreens()
{
    std::array<int, kSeats> seatOn{-1, -1, -1};
    const int  screens = std::clamp(views::column_layout::Views(), 1, kSeats);
    const auto seats   = views::column_layout::Seats();
    const auto slots   = views::column_rules::ScreenSlots(screens, views::column_layout::CurrentOrder(), seats);
    for (int place = 0; place < screens; ++place)
    {
        const int slot = slots[static_cast<std::size_t>(place)];
        if (slot >= 0 && slot < kSeats)
        {
            seatOn[static_cast<std::size_t>(slot)] = seats[static_cast<std::size_t>(place)];
        }
    }
    return seatOn;
}

/// The screen order set and saved (the setting column-order).
void ShowOrder(const views::column_rules::Order& order, const wchar_t* why)
{
    views::column_layout::ChangeOrder(order, why);
    platform::settings::Save(platform::settings::kColumnOrder, rules::OrderWord(order.player));
    g_redraw = true;
}

/// `seat`'s view one screen `dir`: swapped with what is there.
void MoveView(int seat, int dir)
{
    const int  screens = std::clamp(views::column_layout::Views(), 1, kSeats);
    const auto moved   = rules::Moved(SeatOnScreens(), screens, seat, dir);
    const auto order   = moved ? views::column_rules::OrderShowing(*moved) : std::nullopt;
    if (!order)
    {
        platform::LogDebug(L"player window: {}'s view cannot move {} (the row's edge, or one screen)", players::SeatName(seat),
                           dir < 0 ? L"left" : L"right");
        return;
    }
    ShowOrder(*order, L"a player moved their view");
    platform::Log(L"player window: {}'s view moves {}; the screen order is now {} {} {}", players::SeatName(seat),
                  dir < 0 ? L"left" : L"right", order->player[0], order->player[1], order->player[2]);
}

/// `seat`'s player trades numbers with the nearest player `dir`: the two pads trade slots and the two screens trade
/// numbers, so each person keeps their pad and their screen and plays the other number, its character and its window.
/// Nothing in the game changes: its player 1 stays its party leader, now on the other person's pad. Held while a join
/// or a leave is on its way (those name a number).
void TradeNumber(int seat, int dir)
{
    const bool queued = AnyRequest();
    const int  other  = queued ? -1 : rules::TradePartner(seat, dir, g_playing);
    if (other < 0)
    {
        platform::LogDebug(L"player window: {} trades numbers with no one {} ({})", players::SeatName(seat),
                           dir < 0 ? L"lower" : L"higher", queued ? L"a join or leave on its way" : L"the end of the numbers");
        return;
    }
    if (g_joinPending[static_cast<std::size_t>(seat)] || g_joinPending[static_cast<std::size_t>(other)])
    {
        // A join whose player has no member yet is said on player 1's window by its number: the trade waits for it.
        platform::Log(L"player window: {} and {} cannot trade numbers while a join waits for its party member",
                      players::SeatName(seat), players::SeatName(other));
        return;
    }
    const auto table = input::pad_table::Table();
    if (!rules::CanTradePads(table, seat, other))
    {
        platform::Log(L"player window: {} and {} cannot trade numbers: player 1 plays on the keyboard, which stays player "
                      L"1's",
                      players::SeatName(seat), players::SeatName(other));
        return;
    }
    const auto order = views::column_rules::OrderShowing(rules::Traded(SeatOnScreens(), seat, other));
    if (!order || !input::pad_table::Set(rules::WithPadsTraded(table, seat, other), L"two players traded numbers"))
    {
        platform::Log(L"player window: {} and {} cannot trade numbers ({})", players::SeatName(seat),
                      players::SeatName(other), order ? L"the pad table refused the change" : L"no screen order shows it");
        return;
    }
    ShowOrder(*order, L"two players traded numbers");
    std::swap(g_open[static_cast<std::size_t>(seat)], g_open[static_cast<std::size_t>(other)]);
    std::swap(g_notes[static_cast<std::size_t>(seat)], g_notes[static_cast<std::size_t>(other)]);
    ++g_trades;
    platform::Log(L"player window: players {} and {} trade numbers: {} plays player {}, {} player {}; the screen order is "
                  L"now {} {} {}",
                  seat + 1, other + 1, pad::PadName(pad::PadOn(table, seat)), other + 1,
                  pad::PadName(pad::PadOn(table, other)), seat + 1, order->player[0], order->player[1], order->player[2]);
}

/// One pad's input while its player's window is open.
void WindowInput(const Event& e, int seat)
{
    const auto at = static_cast<std::size_t>(e.user);
    if ((e.pressed & pad::kB) != 0)
    {
        g_bSeen[at] = true;
    }
    for (const std::uint16_t button : {rules::kDpadLeft, rules::kDpadRight, rules::kLB, rules::kRB, pad::kA, pad::kStart})
    {
        if ((e.pressed & button) == 0 || !g_open[static_cast<std::size_t>(seat)])
        {
            continue;
        }
        const rules::Action a = rules::OnPress(button);
        if (a.act == rules::Act::Trade)
        {
            TradeNumber(seat, a.dir);
            seat = SeatOfPad(e.user);  // the press's later buttons are the new number's
            if (seat < 0)
            {
                return;
            }
        }
        else if (a.act == rules::Act::Move)
        {
            MoveView(seat, a.dir);
        }
        else if (a.act == rules::Act::Close)
        {
            Close(seat, L"done");
        }
    }
    if (!g_bSeen[at] || !g_open[static_cast<std::size_t>(seat)])
    {
        return;
    }
    const rules::Action b = rules::OnB(seat, e.bTap, e.bHeld);
    if (b.act == rules::Act::Close)
    {
        Close(seat, L"B");
    }
    else if (b.act == rules::Act::Leave || b.act == rules::Act::Refused)
    {
        HoldLeave(seat, e.user);
    }
}

#if LSS_DEV
std::wstring Reading()
{
    std::wstring open;
    for (int seat = 0; seat < kSeats; ++seat)
    {
        if (g_open[static_cast<std::size_t>(seat)])
        {
            open += std::format(L"{}{}", open.empty() ? L"" : L", ", players::SeatName(seat));
        }
    }
    return std::format(L"{}; open: {}; opened {}, joins asked {}, leaves asked {}, number trades {}, players taken back "
                       L"{}, title presses {}{}",
                       g_installed ? L"on" : L"not installed", open.empty() ? L"none" : open, g_opens, g_joinsAsked,
                       g_leavesAsked, g_trades, g_takenBack, g_titlePresses,
                       g_startWhenField >= 0 ? L"; a Start waits for the field" : L"");
}
#endif

}  // namespace

bool Install() noexcept
{
    input::pad_table::Listen(&Note);
    g_installed = true;
    platform::Log(L"player window: Start on a pad that plays no one takes back a player whose pad is gone, else adds a "
                  L"player on it; View held a second opens that player's own window (d-pad left/right: trade player "
                  L"numbers, LB/RB: move the view, A: done, B held: leave); on the title screen the pad that presses "
                  L"plays player 1, or the keyboard, when a key is pressed there (every pad then joins with Start)");
    return true;
}

void SetSession(Session* session) noexcept
{
    g_session = session;
}

void RunRequests() noexcept
try
{
    if (!g_installed || g_session == nullptr || !AnyRequest())
    {
        return;
    }
    if (views::WholeScreenShown())
    {
        return;  // a menu or a scene over the screen: the joins and leaves wait for the field
    }
    for (int& seat : g_leaves)
    {
        if (seat >= 1)
        {
            modules::LeaveSeat(*g_session, seat, L"a player leaves (B held in their window)");
        }
        seat = -1;
    }
    for (JoinAsk& j : g_joins)
    {
        if (j.seat < 0)
        {
            continue;
        }
        input::pad_table::Set(rules::WithPadOn(input::pad_table::Table(), j.user, j.seat), L"a pad joins as a new player");
        g_session->Join(j.seat);
        AnswerJoin(j);
        j = JoinAsk{};
    }
}
catch (...)
{
    platform::Log(L"player window: an exception while joining or leaving; the requests are dropped");
    g_joins  = {};
    g_leaves = {-1, -1, -1};
}

void Remove() noexcept
{
    input::pad_table::Listen(nullptr);
    input::pad_table::SetIdlePads(0, L"the mod's end");
    hud::view_prompt::ClearAll();
    hud::view_window::CloseAll();
    g_open           = {};
    g_notes          = {};
    g_joins          = {};
    g_leaves         = {-1, -1, -1};
    g_startWhenField = -1;
    g_idlePads       = 0;
    g_queued         = 0;
    g_titleClaimedUser = kNoClaim;
    g_keyboardLast   = false;
    g_titleLast      = false;
    g_waiting        = {};
    g_joinPending    = {};
    g_waitSaid       = {};
    g_bSeen          = {};
    g_installed      = false;
}

void Tick(const Frame& frame) noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    const std::size_t               count  = g_queued;
    const std::array<Event, kQueue> events = g_events;  // taken now: anything below that polled could add to it
    g_queued                               = 0;
    if (g_dropped != 0)
    {
        platform::LogDebug(L"player window: {} pad press(es) dropped (more than {} in one tick)", g_dropped, kQueue);
        g_dropped = 0;
    }
    g_playing = frame.seatIds | 1u;
    // The title's notice of which controller is player 1, while it is due; it ends with the title (no line over the
    // loading screen).
    if (hud::save_notice::Active() && !views::AtTitleNow())
    {
        hud::save_notice::End();
    }
    hud::save_notice::Tick();
    WatchKeyboardOnTitle();
    // The title is read only when a press is to be told apart (the press there takes player 1). A press anywhere else
    // ends the title's visit: the next one's first press is said again.
    const bool title = count > 0 && views::AtTitleNow();
    if (count > 0 && !title)
    {
        g_titleClaimedUser = kNoClaim;
    }

    // Players gone (a leave, Ctrl+F8, a field exit) take their windows; one view (a scene, a menu) closes them all.
    for (int seat = 1; seat < kSeats; ++seat)
    {
        if (!Plays(seat))
        {
            Close(seat, L"the player is out");
            g_notes[static_cast<std::size_t>(seat)] = {};
        }
        if (g_waiting[static_cast<std::size_t>(seat)] && Plays(seat) && g_session != nullptr && g_session->In(seat))
        {
            g_waiting[static_cast<std::size_t>(seat)] = false;
            Open(seat, L"its join was waiting for a party member");
        }
        else if (g_waiting[static_cast<std::size_t>(seat)] && g_session != nullptr && !g_session->AwaitingMember(seat) &&
                 !g_session->In(seat))
        {
            g_waiting[static_cast<std::size_t>(seat)] = false;  // the wait was stopped (Ctrl+F8, a field exit)
        }
        PendingJoin(seat, frame);
    }
    if (frame.single)
    {
        for (int seat = 0; seat < kSeats; ++seat)
        {
            Close(seat, L"the views became one (a scene or a menu)");
            if (g_notes[static_cast<std::size_t>(seat)].note != text::Note::None)
            {
                g_notes[static_cast<std::size_t>(seat)] = {};
                g_redraw                                = true;
            }
        }
    }
    const bool pending = std::any_of(g_joinPending.begin(), g_joinPending.end(), [](bool p) { return p; });
    if (frame.single || (pending && views::WholeScreenShown()))
    {
        // A menu or a scene (with or without the views made one): a join still waiting is said again in the field.
        g_waitSaid = {};
    }
    if (frame.screens != g_lastScreens)
    {
        g_lastScreens = frame.screens;
        g_redraw      = true;  // a join's new view, a leave: every window again in its player's view
    }

    // A spare pad's Start remembered over a menu or a scene: the join once the field shows.
    if (g_startWhenField >= 0 && !frame.single && !views::WholeScreenShown())
    {
        const int user   = std::exchange(g_startWhenField, -1);
        SpareStart(user);
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const Event e = events[i];
        if (e.user < 0 || e.user >= pad::kPads)
        {
            continue;
        }
        if (title)
        {
            if (e.pressed != 0)
            {
                TitlePress(e.user, e.pressed);
            }
            continue;
        }
        const int seat = SeatOfPad(e.user);
        if (e.open)
        {
            if (seat < 0 && rules::TakesPlayerOneFromKeyboard(input::pad_table::Table(), g_playing, e.user,
                                                              input::pad_table::TwinOf(e.user, pad::kView) >= 0))
            {
                // While the keyboard plays player 1 (a key on the title, or a stray click there), View held on a pad
                // that plays no one takes player 1 back for that pad.
                if (input::pad_table::Set(rules::WithPlayerOne(input::pad_table::Table(), e.user),
                                          L"View held on a pad while the keyboard played player 1: the pad plays player 1"))
                {
                    platform::Log(L"player window: {} held View: it takes player 1 from the keyboard", pad::PadName(e.user));
                    g_redraw = true;
                }
                continue;
            }
            if (seat < 0)
            {
                continue;  // a pad that plays no one: Start joins
            }
            if (g_open[static_cast<std::size_t>(seat)])
            {
                Close(seat, L"View held again");
            }
            else if (frame.single)
            {
                platform::Log(L"player window: {} held View, but the views are one now (a scene or a menu)",
                              pad::PadName(e.user));
            }
            else
            {
                g_bSeen[static_cast<std::size_t>(e.user)] = false;
                Open(seat, std::format(L"{} held View", pad::PadName(e.user)));
            }
            continue;
        }
        // A pad that plays no one, or whose player waits outside the game (no player there yet): its Start is the mod's
        // (a join, a take-back, or the wait said again).
        if (seat < 0 || (seat >= 1 && g_session != nullptr && !g_session->In(seat)))
        {
            if ((e.pressed & pad::kStart) != 0 && g_session != nullptr)
            {
                SpareStart(e.user);
            }
            continue;
        }
        if (g_open[static_cast<std::size_t>(seat)])
        {
            WindowInput(e, seat);
        }
    }

    // A shown window whose character changed (the member taken just after the join, a party change) shows the new name.
    for (int seat = 0; seat < kSeats && !g_redraw; ++seat)
    {
        const bool shown = g_open[static_cast<std::size_t>(seat)] || g_notes[static_cast<std::size_t>(seat)].note != text::Note::None;
        g_redraw = shown && Plays(seat) && CharacterOf(seat) != g_drawnCharacters[static_cast<std::size_t>(seat)];
    }
    // The game closed a window under a player's (its field-start reset): shown again.
    if (hud::view_window::ClosedByGame() != g_gameClosedSeen)
    {
        g_gameClosedSeen = hud::view_window::ClosedByGame();
        g_redraw         = true;
    }
    // Notes run out.
    const std::int64_t now = NowMs();
    for (SeatNote& n : g_notes)
    {
        if (n.note != text::Note::None && now >= n.until)
        {
            n        = {};
            g_redraw = true;
        }
    }
    SetIdlePads();
    if (std::exchange(g_redraw, false))
    {
        ShowWindows();
    }
}
catch (...)
{
    platform::Log(L"player window: an exception; every window closes");
    g_open  = {};
    g_notes = {};
    hud::view_window::CloseAll();
    hud::view_prompt::ClearAll();
    input::pad_table::SetIdlePads(0, L"the player windows' exception");
    g_idlePads = 0;
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"players.windows",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the players' windows: which are open, and the joins, leaves and title "
                                L"presses the pads asked for",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}

#endif

}  // namespace lss::session::player_window
