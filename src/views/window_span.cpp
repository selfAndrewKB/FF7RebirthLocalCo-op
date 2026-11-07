#include "views/window_span.h"

#include "platform/log.h"
#include "platform/settings.h"
#include "views/columns.h"
#include "views/display_align.h"
#include "views/game_window.h"
#include "views/monitor_picks.h"
#include "views/monitor_row.h"
#include "views/span_desktop.h"
#include "views/window_span_rules.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <vector>

namespace lss::views::window_span {

namespace {

using game_window::kModeFullscreen;
using game_window::kModeWindowed;
using game_window::ModeName;
using game_window::RectText;
using game_window::Reshape;
using game_window::WindowRect;
using monitor_row::Rect;

constexpr std::uint32_t kHoldTicks         = 30;    // the rect and desktop check, twice a second at 60
constexpr std::uint32_t kStatusTicks       = 300;
constexpr std::uint32_t kLineUpTicks       = 120;   // the desktop read as the row after a move; Windows may apply one after the call returns
constexpr std::uint32_t kLineUpCheckTicks  = 6;     // that read is the display configuration: not every tick
constexpr std::uint32_t kPresetGiveUpTicks = 1800;  // the file's preset waits this long for a steady window
constexpr std::int32_t  kModeColumns       = columns::kMaxColumns;  // the monitor mode: one monitor per column

enum class Step
{
    Out,
    LiningUp,  // the row's monitors are moved; the window follows once the desktop reads as the row
    In,
};

bool g_installed = false;

Step          g_step        = Step::Out;
bool          g_held        = false;  // the three-player monitor mode is wanted: spanned, or waiting for its row
bool          g_retry       = false;  // the mode's span refused or lost to the desktop: tried again when the desktop changes
std::uint64_t g_triedShape  = 0;      // the desktop's shape at the last try
std::uint64_t g_rowShape    = 0;      // the desktop's shape the row stands on
std::uint32_t g_outTicks    = 0;
bool          g_rowLost     = false;  // the last leave was the desktop's doing: the next entry keeps the window's place from before
HWND          g_hwnd        = nullptr;
std::int32_t  g_mode        = -1;     // the window's mode at the entry: Windowed keeps the OS border, so its rect is not the row's
Rect          g_before;               // the window rect before the span
Rect          g_target;
std::int32_t  g_columns     = 0;
std::int32_t  g_mostColumns = 0;      // the most columns the row can give: the connected picks, or the desktop's row
std::int32_t  g_rowMonitors = 0;
std::vector<std::int32_t> g_widths;   // the span's monitors' widths, left to right (one per column), while in
std::vector<std::int32_t> g_heights;  // their heights, the same way; the window is as tall as the tallest
std::uint32_t g_ticks       = 0;
std::uint32_t g_stepTicks   = 0;
std::uint32_t g_reapplies   = 0;
std::uint32_t g_reshapes    = 0;      // re-shapes for a change of columns while in

// The file's preset (multi-monitor = on): the monitor mode's one try, once the window has stood still for kHoldTicks
// (the wait itself is the pure rule in views/window_span_rules.h).
bool       g_presetPending = false;
PresetWait g_presetWait;

void Forget() noexcept
{
    g_step        = Step::Out;
    g_hwnd        = nullptr;
    g_mode        = -1;
    g_columns     = 0;
    g_mostColumns = 0;
    g_rowMonitors = 0;
    g_widths.clear();
    g_heights.clear();
    g_ticks     = 0;
    g_stepTicks = 0;
    g_reapplies = 0;
    g_reshapes  = 0;
}

/// The columns a row gives at most: the picks that are connected, else the monitors of the desktop's row.
std::int32_t MostColumns(const span_desktop::Desktop& desktop, const monitor_row::Choice& choice) noexcept
{
    const auto picked = monitor_picks::PickedColumns(desktop.picks);
    return std::min(picked > 0 ? picked : choice.rowMonitors, kModeColumns);
}

/// The window over the row `choice` names for `count` columns, now (the desktop reads as the row). `why` names the
/// step for the log. The span's state changes only once the reshape is made.
bool PlaceOnRow(const game_window::Window& window, const span_desktop::Desktop& desktop, const monitor_row::Choice& choice,
                std::int32_t count, const wchar_t* why) noexcept
try
{
    std::vector<std::int32_t> widths  = choice.widths;  // before anything changes: a failed allocation leaves the state as it was
    std::vector<std::int32_t> heights = choice.heights;
    const Rect seen = WindowRect(window.hwnd);
    if (!Reshape(window.object, choice.span))
    {
        platform::Log(L"window span: {}: ReshapeWindow faulted; the window stays at {}", why, RectText(seen));
        return false;
    }
    const Rect after = WindowRect(window.hwnd);
    g_columns     = count;
    g_mostColumns = MostColumns(desktop, choice);
    g_hwnd        = window.hwnd;
    g_mode        = window.mode;
    g_target      = choice.span;
    g_rowMonitors = choice.rowMonitors;
    g_widths      = std::move(widths);
    g_heights     = std::move(heights);
    g_rowShape    = span_desktop::Shape();
    g_step        = Step::In;
    g_stepTicks   = 0;
    g_rowLost     = false;
    std::wstring sizes;
    for (std::size_t i = 0; i < g_widths.size() && i < g_heights.size(); ++i)
    {
        sizes += std::format(L"{}{}x{}", sizes.empty() ? L"" : L" | ", g_widths[i], g_heights[i]);
    }
    platform::Log(L"window span: {}: {} columns over {}row of {} monitor(s) ({}){}{} ({} mode{}); the engine's "
                  L"ReshapeWindow{}; the window was {}, is now {}{}",
                  why, g_columns, desktop.picks.named > 0 ? L"the settings file's " : choice.gamesRow ? L"the game's " : L"a ",
                  choice.rowMonitors, sizes, L"",
                  display_align::Moved() ? std::format(L" (moved for this Windows session: {})", display_align::Reading())
                                         : std::wstring(),
                  ModeName(window.mode), window.mode == kModeWindowed ? L", so the OS border stays around the client area" : L"",
                  RectText(choice.span), RectText(seen), RectText(after),
                  after == choice.span || window.mode == kModeWindowed ? L"" : L" (NOT the row yet; held from the tick)");
    if (std::adjacent_find(g_widths.begin(), g_widths.end(), std::not_equal_to<>()) != g_widths.end())
    {
        platform::Log(L"window span: the row's monitors differ in width; the engine's equal columns do not end at the "
                      L"monitors' edges");
    }
    return true;
}
catch (...)
{
    platform::LogWarning(L"window span: {}: the entry's log could not be written for lack of memory; the window {} over the row",
                         why, g_step == Step::In ? L"is" : L"is not");
    return g_step == Step::In;
}

/// Spans (or re-spans) the window over up to `count` columns: the row chosen (the settings file's monitors, moved
/// into a row, or one the desktop has), the moves first if any, else the reshape now. `why` names the step for the
/// log.
bool Span(std::int32_t count, const wchar_t* why) noexcept
try
{
    const wchar_t* reason = L"";
    const auto     window = game_window::Find(&reason);
    if (!window)
    {
        platform::Log(L"window span: {}: the game window could not be reached: {}", why, reason);
        return false;
    }
    if (window->mode == kModeFullscreen)
    {
        platform::Log(L"window span: {}: the game is in exclusive fullscreen; set it to windowed fullscreen "
                      L"(borderless) first",
                      why);
        return false;
    }
    const span_desktop::Desktop desktop = span_desktop::Read(window->hwnd);
    // As many columns as the row has monitors, up to the number asked for: two of three picked monitors connected,
    // or a desktop whose longest row is two, span two columns when three are asked.
    const auto choice = span_desktop::WidestRow(desktop, count);
    if (const auto given = static_cast<std::int32_t>(choice.widths.size()); choice.ok && given < count)
    {
        platform::Log(L"window span: {}: {} columns instead of {}: {}", why, given, count,
                      desktop.picks.named > 0
                          ? std::format(L"{} of the {} monitors the settings file names are connected", desktop.picks.found.size(),
                                        desktop.picks.named)
                          : std::format(L"the longest row has {} monitor(s)", choice.rowMonitors));
        count = given;
    }
    if (!choice.ok)
    {
        platform::Log(L"window span: {}: refused: {} ({} monitor(s) listed: {}; the mod changes no display mode or scaling){}",
                      why, choice.why, desktop.list.size(), span_desktop::Text(desktop),
                      g_held ? L"; the three-player monitor mode tries again when the desktop changes" : L"");
        return false;
    }
    if (g_step == Step::Out)
    {
        // A row lost and regained is one stretch of play: Windows may have shifted the window in between, and the place
        // to return to is still the one from before the first entry.
        g_before = g_rowLost ? g_before : WindowRect(window->hwnd);
    }
    if (!choice.moves.empty())
    {
        // Windows applies a move after the call returns and says nothing when it ignores one, so the reshape waits for
        // a tick on which the desktop reads as the row with nothing left to move.
        const auto places = span_desktop::PlacesFor(choice, desktop);
        if (places.size() != choice.moves.size() || !display_align::Apply(places))
        {
            platform::Log(L"window span: {}: refused: {} monitor(s) could not be moved to make the row (see above)", why,
                          choice.moves.size());
            return false;
        }
        g_columns     = count;  // the move's target; the window follows once the desktop reads as the row
        g_mostColumns = MostColumns(desktop, choice);
        g_hwnd        = window->hwnd;
        g_step        = Step::LiningUp;
        g_stepTicks   = 0;
        platform::Log(L"window span: {}: moving monitors to make the row ({}); the window follows once the desktop reads "
                      L"as the row; the desktop before: {}",
                      why, display_align::Reading(), span_desktop::Text(desktop));
        return true;
    }
    return PlaceOnRow(*window, desktop, choice, count, why);
}
catch (...)
{
    platform::LogWarning(L"window span: {}: the entry failed for lack of memory", why);
    return false;
}

/// With the monitors back: a window whose old place is on no monitor goes to the primary, not off the desktop.
void KeepWindowOnDesktop() noexcept
try
{
    const RECT before{g_before.left, g_before.top, g_before.right, g_before.bottom};
    if (MonitorFromRect(&before, MONITOR_DEFAULTTONULL) != nullptr)
    {
        return;
    }
    const POINT corner{0, 0};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(corner, MONITOR_DEFAULTTOPRIMARY), &info))
    {
        platform::Log(L"window span: the window's old place {} is on no monitor now; it goes to the primary", RectText(g_before));
        g_before = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom};
    }
}
catch (...)
{
}

/// The monitors put back first, then the window, since its old place is a rect of the desktop they came from; the
/// span's state forgotten. The mode's wish is the caller's.
void LeaveSpan(const wchar_t* why) noexcept
try
{
    display_align::Restore();
    KeepWindowOnDesktop();
    const wchar_t* reason = L"";
    const auto     window = game_window::Find(&reason);
    const bool     back   = window && Reshape(window->object, g_before);
    const Rect     after  = window ? WindowRect(window->hwnd) : Rect{};
    platform::Log(L"window span: {}: the window {} {}{}; re-applied {} time(s), re-shaped for other columns {} time(s) "
                  L"while in",
                  why, back ? L"put back to" : L"could NOT be put back to", RectText(g_before),
                  window ? std::format(L", now {}", RectText(after)) : std::format(L" ({})", reason), g_reapplies,
                  g_reshapes);
    Forget();
}
catch (...)
{
    platform::LogWarning(L"window span: {}: the leave's log could not be written for lack of memory", why);
    Forget();
}

/// A span that ended without the player's word: the held mode tries again when the desktop changes.
void RetryWhenTheDesktopChanges() noexcept
{
    g_retry      = g_held;
    g_triedShape = span_desktop::Shape();
}

/// The monitors were moved: the row is entered once the desktop reads as the row, and given up after a while.
void LineUpTick() noexcept
try
{
    if (g_stepTicks % kLineUpCheckTicks != 0 && g_stepTicks <= kLineUpTicks)
    {
        return;
    }
    const wchar_t* reason = L"";
    const auto     window = game_window::Find(&reason);
    if (!window)
    {
        platform::LogWarning(L"window span: the game window could not be reached during the move ({}); the monitors are put back",
                             reason);
        display_align::Restore();
        Forget();
        RetryWhenTheDesktopChanges();
        return;
    }
    const auto desktop = span_desktop::Read(window->hwnd);
    const auto choice  = span_desktop::Row(desktop, g_columns);
    if (choice.ok && choice.moves.empty())
    {
        platform::Log(L"window span: the desktop reads as the row after {} tick(s)", g_stepTicks);
        if (!PlaceOnRow(*window, desktop, choice, g_columns, L"the monitors in place"))
        {
            LeaveSpan(L"the reshape after the move faulted");
            RetryWhenTheDesktopChanges();
        }
    }
    else if (g_stepTicks > kLineUpTicks)
    {
        platform::LogWarning(L"window span: after the move the desktop still does not read as the row ({}) after {} ticks; "
                             L"the monitors are put back; the desktop now: {}",
                             choice.ok ? std::format(L"{} move(s) still listed", choice.moves.size()) : choice.why, g_stepTicks,
                             span_desktop::Text(desktop));
        LeaveSpan(L"the move did not take");
        RetryWhenTheDesktopChanges();
    }
}
catch (...)
{
    platform::LogWarning(L"window span: the desktop could not be read during the move; the monitors are put back");
    display_align::Restore();
    Forget();
    RetryWhenTheDesktopChanges();
}

/// The file's preset: once the game window reads the same rect for kHoldTicks ticks (the engine has sized it to the
/// saved resolution), the one try of the monitor mode; given up, logged, if no steady window comes.
void PresetTick() noexcept
try
{
    const wchar_t* reason = L"";
    const auto     window = game_window::Find(&reason);
    const auto     seen   = window ? std::optional(WindowRect(window->hwnd)) : std::nullopt;
    switch (g_presetWait.Next(seen, kHoldTicks, kPresetGiveUpTicks))
    {
        case PresetWait::Step::Wait: return;
        case PresetWait::Step::Try:
            g_presetPending = false;
            platform::Log(L"window span: multi-monitor = on: the game window is steady at {} after {} tick(s); the "
                          L"three-player monitor mode is tried now",
                          RectText(g_presetWait.rect), g_presetWait.ticks);
            Hold(true);  // logs its outcome; with no row now the mode waits for one
            return;
        case PresetWait::Step::GiveUp:
            g_presetPending = false;
            platform::LogWarning(L"window span: multi-monitor = on: no steady game window within {} ticks ({}); the "
                                 L"three-player monitor mode stays off (Ctrl+F3 or 'multi-monitor on' tries it)",
                                 g_presetWait.ticks, window ? L"the window kept moving" : reason);
            return;
    }
}
catch (...)
{
    g_presetPending = false;
    platform::LogWarning(L"window span: multi-monitor = on: the try's log could not be written for lack of memory; the mode stays off");
}

/// The desktop's shape changed while the row stands: leaves unless the row still stands as it did; the held mode
/// comes back on its own.
void CheckTheDesktop() noexcept
try
{
    g_rowShape = span_desktop::Shape();
    const auto desktop = span_desktop::Read(g_hwnd);
    // The held mode takes every picked monitor that is connected, so one that arrives is taken in; the span by the
    // players keeps its columns.
    const auto picked = monitor_picks::PickedColumns(desktop.picks);
    const auto wanted = g_held && desktop.picks.named > 0 ? std::min(picked, kModeColumns) : g_columns;
    const auto choice = span_desktop::Row(desktop, wanted > 0 ? wanted : g_columns);
    const bool same   = choice.ok && choice.moves.empty() && choice.span == g_target && (wanted == 0 || wanted == g_columns);
    if (same)
    {
        g_mostColumns = MostColumns(desktop, choice);  // a monitor added to the row's end lets the span grow with the players
        platform::Log(L"window span: the desktop changed and the row still stands: {}", span_desktop::Text(desktop));
        return;
    }
    platform::Log(L"window span: the desktop changed and the row no longer stands ({}): {}",
                  choice.ok ? L"another row is wanted" : choice.why, span_desktop::Text(desktop));
    LeaveSpan(L"the desktop changed");
    g_retry      = g_held;
    g_triedShape = 0;       // the first try right after the leave
    g_rowLost    = g_held;  // the held mode comes back to a row and keeps the window's place from before; a span by the players ends here
}
catch (...)
{
    platform::LogWarning(L"window span: the changed desktop could not be read");
}

/// Twice a second while in: the desktop compared with the row's, then the window put back over the row if anything
/// moved it.
void HoldRect() noexcept
try
{
    if (g_ticks % kHoldTicks != 0 || !IsWindow(g_hwnd))
    {
        return;
    }
    if (span_desktop::Shape() != g_rowShape)
    {
        CheckTheDesktop();
        return;
    }
    const Rect seen = WindowRect(g_hwnd);
    if (seen == g_target || g_mode == kModeWindowed)  // a Windowed window's rect holds the OS border: not the row's
    {
        return;
    }
    const wchar_t* why    = L"";
    const auto     window = game_window::Find(&why);
    const bool     done   = window && Reshape(window->object, g_target);
    ++g_reapplies;
    platform::Log(L"window span: the window was at {}, not the row; {} (re-apply {})", RectText(seen),
                  done ? L"put back through ReshapeWindow" : L"could NOT be put back", g_reapplies);
}
catch (...)
{
}

/// Out: the mode's span, refused or lost, is tried again when the desktop changes.
void StepOut() noexcept
{
    if (g_held && g_retry && ++g_outTicks % kHoldTicks == 0 && span_desktop::Shape() != g_triedShape)
    {
        g_triedShape = span_desktop::Shape();
        platform::Log(L"window span: the desktop changed; the three-player monitor mode tries its row again");
        g_retry = !Enter(kModeColumns);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!game_window::Install(image))
    {
        return false;
    }
    g_installed = true;
    display_align::HealEarlierRun();
    platform::Log(L"window span: ready (Ctrl+F2 or 'window-span on' while player 2 or 3 is in: one monitor per column; "
                  L"Ctrl+F3 or 'multi-monitor on': the three-player monitor mode, the row whatever the players; the row "
                  L"is the settings file's monitors, moved into one while the game holds it, or one the desktop has; "
                  L"the engine's own window reshape)");
    SetPreset(platform::settings::Of(platform::settings::kMultiMonitor).word,
              platform::settings::Why(platform::settings::kMultiMonitor).c_str());
    return true;
}

bool Installed() noexcept
{
    return g_installed;
}

bool In() noexcept
{
    return g_step != Step::Out;
}

bool Enter(std::int32_t count) noexcept
{
    if (!g_installed)
    {
        platform::Log(L"window span: not installed");
        return false;
    }
    if (In())
    {
        platform::Log(L"window span: already in ({} columns)", g_columns);
        return false;
    }
    if (count < 2 || count > columns::kMaxColumns)
    {
        platform::Log(L"window span: {} column(s); the span takes 2 to {} (one monitor per column)", count,
                      columns::kMaxColumns);
        return false;
    }
    if (!Span(count, L"in"))
    {
        display_align::Restore();
        Forget();
        return false;
    }
    return true;
}

void Leave(const wchar_t* why) noexcept
{
    const bool held = g_held;
    g_held    = false;
    g_retry   = false;
    g_rowLost = false;
    if (!In())
    {
        if (held)
        {
            platform::Log(L"window span: {}: the three-player monitor mode is off (it was waiting for its row)", why);
        }
        return;
    }
    LeaveSpan(why);
    if (held)
    {
        platform::Log(L"window span: the three-player monitor mode is off");
    }
}

bool Hold(bool on) noexcept
{
    g_presetPending = false;  // either way the file's try is answered: off beats it, on is it
    if (!on)
    {
        if (g_held)
        {
            Leave(L"the three-player monitor mode is switched off");
        }
        return true;
    }
    if (g_held && In())
    {
        return true;
    }
    if (!g_installed)
    {
        platform::Log(L"window span: not installed; no three-player monitor mode");
        return false;
    }
    g_held = true;
    const bool spanned = In() ? (g_columns == kModeColumns || g_mostColumns == g_columns ||
                                 Span(kModeColumns, L"the three-player monitor mode"))
                              : Enter(kModeColumns);
    if (!spanned)
    {
        RetryWhenTheDesktopChanges();
        platform::Log(L"window span: the three-player monitor mode is on and waits for its row (see above): tried again "
                      L"when the desktop changes; Ctrl+F3 or 'multi-monitor off' ends it");
        return false;
    }
    g_retry = false;
    platform::Log(L"window span: the three-player monitor mode is on: {} columns held whatever the players; the "
                  L"mirror views fill the columns no player has",
                  g_columns);
    return true;
}

bool Held() noexcept
{
    return g_held;
}

std::int32_t HeldColumns() noexcept
{
    return g_held && g_step == Step::In ? g_columns : 0;  // not while the monitors move: the window is one monitor wide still
}

void SetPreset(const std::wstring& word, const wchar_t* why) noexcept
{
    if (word == L"on")
    {
        if (!g_installed)
        {
            platform::Log(L"window span: multi-monitor = on ({}): the window span is not installed, so the three-player "
                          L"monitor mode stays off",
                          why);
            return;
        }
        if (In())
        {
            platform::Log(L"window span: multi-monitor = on ({}): the three-player monitor mode now", why);
            Hold(true);
            return;
        }
        g_presetPending = true;
        g_presetWait    = PresetWait{};
        platform::Log(L"window span: multi-monitor = on ({}): the three-player monitor mode from the game's first steady "
                      L"frame, whatever the players; player 1's picture fills every column no player has",
                      why);
    }
    else if (word == L"off")
    {
        platform::Log(L"window span: multi-monitor = off ({}): no span until window-span on or Ctrl+F2 with a player in, "
                      L"or Ctrl+F3",
                      why);
        Hold(false);
    }
    else
    {
        platform::LogWarning(L"window span: multi-monitor = '{}' ({}) is not a word of the table; nothing changes", word, why);
    }
}

std::wstring Preset()
{
    return g_held || g_presetPending ? L"on" : L"off";
}

void Tick(std::int32_t wanted) noexcept
{
    if (g_presetPending)
    {
        PresetTick();
    }
    if (!In())
    {
        StepOut();
        return;
    }
    ++g_ticks;
    ++g_stepTicks;
    if (g_step == Step::LiningUp)
    {
        LineUpTick();
        return;
    }
    if (!g_held && wanted < 2)
    {
        Leave(L"one column is left (no other player is in)");
        return;
    }
    // The window follows the columns as far as the row reaches: a row of two monitors holds two columns for three
    // players (the engine lays the third out in the window all the same).
    const std::int32_t target = std::min(wanted, g_mostColumns);
    if (target >= 2 && target != g_columns && target <= columns::kMaxColumns)
    {
        const std::int32_t was = g_columns;
        ++g_reshapes;
        bool followed = false;
        try
        {
            followed = Span(target, std::format(L"{} columns instead of {}", target, was).c_str());
        }
        catch (...)
        {
        }
        if (!followed)
        {
            LeaveSpan(L"the window could not follow the columns");
            RetryWhenTheDesktopChanges();
        }
        return;
    }
    HoldRect();
    if (g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"window span: {}", Reading());
        }
        catch (...)
        {
        }
    }
}

std::span<const std::int32_t> ColumnWidths() noexcept
{
    return g_step == Step::In ? std::span<const std::int32_t>(g_widths) : std::span<const std::int32_t>();
}

std::span<const std::int32_t> ColumnHeights() noexcept
{
    return g_step == Step::In ? std::span<const std::int32_t>(g_heights) : std::span<const std::int32_t>();
}

std::wstring Reading()
{
    if (!g_installed)
    {
        return L"unavailable";
    }
    if (!In())
    {
        return g_held ? L"out; the three-player monitor mode waits for its row" : L"out";
    }
    return std::format(L"{}{}: {} columns (the row gives {} at most), the row has {} monitor(s), the window {} (the row {}), "
                       L"re-applies {}, re-shapes {}{}",
                       g_step == Step::LiningUp ? L"moving monitors" : L"in", g_held ? L" (the three-player monitor mode)" : L"",
                       g_columns, g_mostColumns, g_rowMonitors, IsWindow(g_hwnd) ? RectText(WindowRect(g_hwnd)) : std::wstring(L"?"),
                       RectText(g_target), g_reapplies, g_reshapes,
                       display_align::Moved() ? std::format(L"; monitors moved: {}", display_align::Reading()) : std::wstring());
}

}  // namespace lss::views::window_span
