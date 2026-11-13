#include "session/player_window.h"

#include "facts/controller.h"
#include "hud/save_notice.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "input/pad_rules.h"
#include "input/pad_table.h"
#include "modules.h"
#include "platform/guarded_read.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/actor_fields.h"
#include "players/describe.h"
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
};

constexpr std::int64_t kNoteMs              = 3000;
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
std::array<bool, pad::kSlots> g_bSeen{};             // by pad: B went down while its window was open (arms B)
std::array<JoinAsk, kSeats>   g_joins{};             // asked, not yet made
std::array<int, kSeats>       g_leaves{-1, -1, -1};  // players asked to leave, not yet out
std::array<bool, kSeats>      g_waiting{};           // by seat: a join that waits; its window opens once in
int                           g_startWhenField = -1;  // a spare pad's Start during a menu or a scene: joins after
bool                          g_redraw         = false;
std::uint32_t                 g_playing        = 1;  // the slots a player holds, as of the last tick
std::uint32_t                 g_idleUsers      = 0;  // the pads read idle now (the open windows' pads)
int                           g_lastScreens    = 0;  // the views as last shown
unsigned long long            g_gameClosedSeen = 0;  // view_window's count of windows the game closed
std::uint32_t                 g_opens = 0, g_joinsAsked = 0, g_leavesAsked = 0, g_titlePresses = 0;
int                           g_titleClaimedUser = -1;  // the pad the title's notice last named (-1: none since the title)
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
    return seat == 0 || (seat > 0 && seat < kSeats && (g_playing & (1u << seat)) != 0);
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
    g_notes[static_cast<std::size_t>(seat)] = {note, about, NowMs() + kNoteMs};
    g_redraw                                = true;
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
        w.canMove    = g_lastScreens >= 2;
        w.startJoins = seat == 0 && FreeSeat() >= 0;
        w.note       = note.note;
        w.about      = note.about;
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
    std::uint32_t users = 0;
    const auto    table = input::pad_table::Table();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        if (g_open[static_cast<std::size_t>(seat)])
        {
            users |= 1u << table[static_cast<std::size_t>(seat)];
        }
    }
    if (users != g_idleUsers)
    {
        g_idleUsers = users;
        input::pad_table::SetIdleUsers(users, L"the open player windows");
    }
}

/// A spare pad's Start: a join on the lowest seat no player holds (during a menu or a scene, once the field shows).
void AskJoin(int user)
{
    if (std::any_of(g_joins.begin(), g_joins.end(), [user](const JoinAsk& j) { return j.user == user; }))
    {
        return;  // asked already: its join is made before the session's next tick
    }
    if (const int slot = pad::SlotOf(input::pad_table::Table(), user);
        slot >= 1 && slot < kSeats && g_session != nullptr && modules::SeatTaken(*g_session, slot))
    {
        platform::Log(L"player window: pad {} pressed Start, but its player is already joining", user + 1);
        return;  // its seat waits for a member: a second Start never takes another seat
    }
    const int free = FreeSeat();
    if (free < 0)
    {
        SetNote(0, text::Note::AllIn, 0);
        platform::Log(L"player window: pad {} pressed Start, but every player is in or on the way", user + 1);
        return;
    }
    if (views::WholeScreenShown())
    {
        g_startWhenField = user;
        platform::Log(L"player window: pad {} pressed Start during a menu or a scene; it joins once the field shows",
                      user + 1);
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
    platform::Log(L"player window: pad {} pressed Start: it joins as {}", user + 1, players::SeatName(free));
}

/// The session's answer to a join: in (its window opens), or waiting for a member (said on player 1's window).
void AnswerJoin(const JoinAsk& j)
{
    g_redraw                                  = true;
    g_bSeen[static_cast<std::size_t>(j.user)] = false;  // a B from before the join never closes or leaves
    if (g_session->In(j.seat))
    {
        g_open[static_cast<std::size_t>(j.seat)] = true;
        platform::Log(L"player window: {} is in on pad {}; its window opens", players::SeatName(j.seat), j.user + 1);
        return;
    }
    if (g_session->AwaitingMember(j.seat))
    {
        g_waiting[static_cast<std::size_t>(j.seat)] = true;
        SetNote(0, text::Note::Waits, j.seat);
        platform::Log(L"player window: {} joins once a party member is free", players::SeatName(j.seat));
        return;
    }
    SetNote(0, text::Note::Failed, j.seat);
    platform::Log(L"player window: {}'s join failed (see the session's lines above)", players::SeatName(j.seat));
}

/// B held on `seat`'s pad with its window open: player 2 or 3 leaves; player 1 is refused.
void HoldLeave(int seat, int user)
{
    if (seat == 0)
    {
        SetNote(0, text::Note::PlayerOneStays, 0);
        platform::Log(L"player window: pad {} held B, but player 1 stays in", user + 1);
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
        platform::Log(L"player window: pad {} held B: {} leaves", user + 1, players::SeatName(seat));
    }
}

/// On the title screen: the pad that pressed becomes player 1's, said on the game's save-data notice (the title's one
/// notice) for a few seconds, once per pad that takes the place (its own later presses, the title menu's, say
/// nothing more; another pad's press says that pad).
void TitlePress(int user)
{
    const auto table = input::pad_table::Table();
    if (pad::SlotOf(table, user) != 0)
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

/// `seat`'s view one screen `dir`: swapped with what is there, the screen order changed and saved.
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
    views::column_layout::ChangeOrder(*order, L"a player moved their view");
    platform::settings::Save(platform::settings::kColumnOrder, rules::OrderWord(order->player));
    g_redraw = true;
    platform::Log(L"player window: {}'s view moves {}; the screen order is now {} {} {}", players::SeatName(seat),
                  dir < 0 ? L"left" : L"right", order->player[0], order->player[1], order->player[2]);
}

/// One pad's input while its player's window is open.
void WindowInput(const Event& e, int seat)
{
    const auto at = static_cast<std::size_t>(e.user);
    if ((e.pressed & pad::kB) != 0)
    {
        g_bSeen[at] = true;
    }
    for (const std::uint16_t button : {rules::kLB, rules::kRB, pad::kA, pad::kStart})
    {
        if ((e.pressed & button) == 0 || !g_open[static_cast<std::size_t>(seat)])
        {
            continue;
        }
        const rules::Action a = rules::OnPress(button);
        if (a.act == rules::Act::Move)
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
    return std::format(L"{}; open: {}; opened {}, joins asked {}, leaves asked {}, title presses {}{}",
                       g_installed ? L"on" : L"not installed", open.empty() ? L"none" : open, g_opens, g_joinsAsked,
                       g_leavesAsked, g_titlePresses, g_startWhenField >= 0 ? L"; a Start waits for the field" : L"");
}
#endif

}  // namespace

bool Install() noexcept
{
    input::pad_table::Listen(&Note);
    g_installed = true;
    platform::Log(L"player window: Start on a pad that plays no one adds a player on it; View held a second opens that "
                  L"player's own window (A: done, B held: leave); on the title screen the pad that presses plays "
                  L"player 1");
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
    input::pad_table::SetIdleUsers(0, L"the mod's end");
    hud::view_prompt::ClearAll();
    hud::view_window::CloseAll();
    g_open           = {};
    g_notes          = {};
    g_joins          = {};
    g_leaves         = {-1, -1, -1};
    g_startWhenField = -1;
    g_idleUsers      = 0;
    g_queued         = 0;
    g_titleClaimedUser = -1;
    g_waiting        = {};
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
    // The title is read only when a press is to be told apart (the press there takes player 1). A press anywhere else
    // ends the title's visit: the next one's first press is said again.
    const bool title = count > 0 && views::AtTitleNow();
    if (count > 0 && !title)
    {
        g_titleClaimedUser = -1;
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
    if (frame.screens != g_lastScreens)
    {
        g_lastScreens = frame.screens;
        g_redraw      = true;  // a join's new view, a leave: every window again in its player's view
    }

    // A spare pad's Start remembered over a menu or a scene: the join once the field shows.
    if (g_startWhenField >= 0 && !frame.single && !views::WholeScreenShown())
    {
        const int user   = std::exchange(g_startWhenField, -1);
        AskJoin(user);
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const Event e = events[i];
        if (e.user < 0 || e.user >= pad::kSlots)
        {
            continue;
        }
        if (title)
        {
            if (e.pressed != 0)
            {
                TitlePress(e.user);
            }
            continue;
        }
        const int seat = rules::SeatOf(input::pad_table::Table(), g_playing, e.user);
        if (e.open)
        {
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
                platform::Log(L"player window: pad {} held View, but the views are one now (a scene or a menu)", e.user + 1);
            }
            else
            {
                g_bSeen[static_cast<std::size_t>(e.user)] = false;
                Open(seat, std::format(L"pad {} held View", e.user + 1));
            }
            continue;
        }
        if (seat < 0)
        {
            if ((e.pressed & pad::kStart) != 0 && g_session != nullptr)
            {
                AskJoin(e.user);
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
    input::pad_table::SetIdleUsers(0, L"the player windows' exception");
    g_idleUsers = 0;
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
