#include "views/window_span.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/unreal_access.h"
#include "views/columns.h"
#include "views/display_align.h"
#include "views/monitor_row.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UObjectGlobals.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::views::window_span {

namespace {

using monitor_row::Rect;

// FGenericWindow's virtuals the module calls, by their stock 4.26 slot (GenericWindow.h; the research confirmed the
// shipped table keeps the order).
using ReshapeWindowFn     = void (*)(void* self, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
using GetWindowModeFn     = std::int32_t (*)(void* self);
using GetOSWindowHandleFn = void* (*)(void* self);

constexpr std::int32_t  kModeFullscreen         = 0;  // EWindowMode::Fullscreen
constexpr std::int32_t  kModeWindowedFullscreen = 1;
constexpr std::int32_t  kModeWindowed           = 2;
constexpr std::uint32_t kHoldTicks              = 30;   // the rect check, twice a second at 60
constexpr std::uint32_t kStatusTicks            = 300;
constexpr std::uint32_t kLineUpTicks            = 120;  // the desktop read as the row after a move (Remake: 2 ms was too soon)
constexpr std::int32_t  kModeColumns            = columns::kMaxColumns;  // the monitor mode: one monitor per column

enum class Step
{
    Out,
    LiningUp,  // a row monitor moved to the row's top; the window follows once the desktop reads as the row
    In,
};

bool           g_installed = false;
std::uintptr_t g_engineGlobal = 0;   // GEngine, live
std::uintptr_t g_vtable       = 0;   // FWindowsWindow's vtable, live
std::uintptr_t g_viewportWindowOffset = 0;  // UGameEngine::GameViewportWindow
std::uintptr_t g_nativeWindowOffset   = 0;  // SWindow::NativeWindow

Step          g_step      = Step::Out;
bool          g_held      = false;     // the three-player monitor mode
HWND          g_hwnd      = nullptr;
Rect          g_before;                // the window rect before the span
Rect          g_target;
std::int32_t  g_columns   = 0;
std::int32_t  g_rowMonitors = 0;
std::uint32_t g_ticks     = 0;
std::uint32_t g_stepTicks = 0;
std::uint32_t g_reapplies = 0;
std::uint32_t g_reshapes  = 0;         // re-shapes for a change of columns while in

/// The game's native window: GEngine -> GameViewportWindow (TWeakPtr<SWindow>: the object pointer first) ->
/// SWindow::NativeWindow (TSharedPtr: the object pointer first). Every read guarded; the vtable must be the one
/// the research found, and its GetOSWindowHandle must name a top-level window of this process.
struct NativeWindow
{
    void*        object = nullptr;
    HWND         hwnd   = nullptr;
    std::int32_t mode   = -1;
};

std::optional<std::uintptr_t> ReadPointer(std::uintptr_t at) noexcept
{
    std::uintptr_t value = 0;
    return platform::Read(at, value) ? std::optional(value) : std::nullopt;
}

template <class Fn>
Fn Slot(std::size_t index) noexcept
{
    const auto entry = ReadPointer(g_vtable + index * sizeof(void*));
    return entry ? reinterpret_cast<Fn>(*entry) : nullptr;
}

std::optional<NativeWindow> FindNativeWindow(const wchar_t** why) noexcept
{
    const auto engine = ReadPointer(g_engineGlobal);
    if (!engine || *engine == 0)
    {
        *why = L"GEngine is not set";
        return std::nullopt;
    }
    const auto swindow = ReadPointer(*engine + g_viewportWindowOffset);
    if (!swindow || *swindow == 0)
    {
        *why = L"UGameEngine::GameViewportWindow is empty";
        return std::nullopt;
    }
    const auto native = ReadPointer(*swindow + g_nativeWindowOffset);
    if (!native || *native == 0)
    {
        *why = L"SWindow::NativeWindow is empty";
        return std::nullopt;
    }
    const auto vtable = ReadPointer(*native);
    if (!vtable || *vtable != g_vtable)
    {
        *why = L"the native window's vtable is not FWindowsWindow's";
        return std::nullopt;
    }
    NativeWindow window;
    window.object = reinterpret_cast<void*>(*native);
    void* handle  = nullptr;
    if (!platform::CallGuarded(Slot<GetOSWindowHandleFn>(facts::kWindowsWindowSlotGetOSWindowHandle), handle,
                               window.object))
    {
        *why = L"GetOSWindowHandle faulted";
        return std::nullopt;
    }
    window.hwnd = static_cast<HWND>(handle);
    DWORD owner = 0;
    GetWindowThreadProcessId(window.hwnd, &owner);
    if (!IsWindow(window.hwnd) || owner != GetCurrentProcessId() || GetParent(window.hwnd) != nullptr)
    {
        *why = L"GetOSWindowHandle is not a top-level window of this process";
        return std::nullopt;
    }
    if (!platform::CallGuarded(Slot<GetWindowModeFn>(facts::kWindowsWindowSlotGetWindowMode), window.mode,
                               window.object))
    {
        *why = L"GetWindowMode faulted";
        return std::nullopt;
    }
    return window;
}

Rect WindowRect(HWND hwnd) noexcept
{
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    return {rect.left, rect.top, rect.right, rect.bottom};
}

std::wstring RectText(const Rect& r)
{
    return std::format(L"({}, {}, {}, {})", r.left, r.top, r.right, r.bottom);
}

const wchar_t* ModeName(std::int32_t mode) noexcept
{
    switch (mode)
    {
        case kModeFullscreen: return L"fullscreen";
        case kModeWindowedFullscreen: return L"windowed fullscreen";
        case kModeWindowed: return L"windowed";
        default: return L"unknown";
    }
}

/// Every monitor with its effective DPI and its device name, and the index of the game window's.
struct Monitors
{
    std::vector<monitor_row::Monitor> list;
    std::vector<std::wstring>         devices;  // GDI device names, as display_align takes them
    HMONITOR                          own    = nullptr;
    std::size_t                       gameAt = 0;  // list.size() when the game's monitor is not listed
};

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param) noexcept
{
    auto*          out = reinterpret_cast<Monitors*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
    {
        return TRUE;
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpi  = reinterpret_cast<GetDpiForMonitorFn>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
    if (getDpi != nullptr)
    {
        getDpi(handle, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    if (handle == out->own)
    {
        out->gameAt = out->list.size();
    }
    out->list.push_back({{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom}, dpiX});
    out->devices.emplace_back(info.szDevice);
    return TRUE;
}

/// The desktop now, with the game window's monitor marked.
Monitors Collect(HWND game) noexcept
{
    Monitors monitors;
    try
    {
        monitors.own    = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
        monitors.gameAt = SIZE_MAX;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    }
    catch (...)
    {
        // An allocation failure: the list as far as it got.
    }
    if (monitors.gameAt == SIZE_MAX || monitors.gameAt > monitors.list.size())
    {
        monitors.gameAt = monitors.list.size();
    }
    return monitors;
}

/// Every monitor as read: device, rect, DPI, the game's marked.
std::wstring DesktopText(const Monitors& monitors)
{
    std::wstring text;
    for (std::size_t i = 0; i < monitors.list.size(); ++i)
    {
        const auto& m = monitors.list[i];
        text += std::format(L"{}{} {}{} {} dpi", i ? L", " : L"", monitors.devices[i], RectText(m.rect),
                            i == monitors.gameAt ? L" (the game's)" : L"", m.dpi);
    }
    return text;
}

/// The choice's moves as the display API takes them: each monitor by the device name its rect was listed with.
std::vector<display_align::Move> MovesFor(const monitor_row::Choice& choice, const Monitors& monitors)
{
    std::vector<display_align::Move> moves;
    for (const auto& move : choice.moves)
    {
        for (std::size_t i = 0; i < monitors.list.size(); ++i)
        {
            if (monitors.list[i].rect == move.from)
            {
                moves.push_back({monitors.devices[i], move.toTop});
            }
        }
    }
    return moves;
}

bool Reshape(void* native, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), native, rect.left,
                                 rect.top, rect.Width(), rect.Height());
}

void Forget() noexcept
{
    g_step      = Step::Out;
    g_held      = false;
    g_hwnd      = nullptr;
    g_columns   = 0;
    g_rowMonitors = 0;
    g_ticks     = 0;
    g_stepTicks = 0;
    g_reapplies = 0;
    g_reshapes  = 0;
}

/// The window over the row `choice` names for `count` columns, now (the desktop reads as the row). `why` names the
/// step for the log. The span's state changes only once the reshape is made.
bool PlaceOnRow(const NativeWindow& window, const monitor_row::Choice& choice, std::int32_t count,
                const wchar_t* why) noexcept
{
    const Rect seen = WindowRect(window.hwnd);
    if (!Reshape(window.object, choice.span))
    {
        platform::Log(L"window span: {}: ReshapeWindow faulted; the window stays at {}", why, RectText(seen));
        return false;
    }
    const Rect after = WindowRect(window.hwnd);
    g_columns     = count;
    g_hwnd        = window.hwnd;
    g_target      = choice.span;
    g_rowMonitors = choice.rowMonitors;
    g_step        = Step::In;
    g_stepTicks   = 0;
    platform::Log(L"window span: {}: {} columns over {} of the {} equal monitor(s) of {}row ({} mode{}){}; the "
                  L"engine's ReshapeWindow{}; the window was {}, is now {}{}",
                  why, g_columns, g_columns, choice.rowMonitors, choice.gamesRow ? L"the game's " : L"a ",
                  ModeName(window.mode), window.mode == kModeWindowed ? L", so the OS border stays around the client area" : L"",
                  display_align::Moved() ? std::format(L" (lined up for this Windows session: {})", display_align::Reading())
                                         : std::wstring(),
                  RectText(choice.span), RectText(seen), RectText(after),
                  after == choice.span || window.mode == kModeWindowed ? L"" : L" (NOT the row yet; held from the tick)");
    return true;
}

/// Spans (or re-spans) the window over `columns` monitors: the row chosen, a line-up first if a monitor sits a few
/// pixels off, else the reshape now. `why` names the step for the log.
bool Span(std::int32_t count, const wchar_t* why) noexcept
{
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
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
    const Monitors monitors = Collect(window->hwnd);
    const auto     choice   = monitor_row::Choose(monitors.list, monitors.gameAt, count);
    if (!choice.ok)
    {
        platform::Log(L"window span: {}: refused: {} ({} monitor(s) listed; the mod lines up a row's tops itself within "
                      L"{} px and changes no display mode or scaling: arrange equal monitors in a row at 100 % "
                      L"scaling)",
                      why, choice.why, monitors.list.size(), monitor_row::kAlignTolerance);
        return false;
    }
    if (g_step == Step::Out)
    {
        g_before = WindowRect(window->hwnd);
    }
    if (!choice.moves.empty())
    {
        // Windows applies the move after the call returns (Remake: the desktop still read the old layout 2 ms later),
        // so the reshape waits for a tick on which the desktop reads as the row with nothing left to move.
        const auto moves = MovesFor(choice, monitors);
        if (moves.size() != choice.moves.size() || !display_align::Apply(moves))
        {
            platform::Log(L"window span: {}: refused: {} monitor(s) of the row could not be lined up with its top (see "
                          L"above)",
                          why, choice.moves.size());
            return false;
        }
        g_columns   = count;  // the line-up's target; the window follows once the desktop reads as the row
        g_hwnd      = window->hwnd;
        g_step      = Step::LiningUp;
        g_stepTicks = 0;
        platform::Log(L"window span: {}: lining up the row ({}); the window follows once the desktop reads as the "
                      L"row; the desktop before: {}",
                      why, display_align::Reading(), DesktopText(monitors));
        return true;
    }
    return PlaceOnRow(*window, choice, count, why);
}

void LineUpTick() noexcept
{
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    if (!window)
    {
        Leave(std::format(L"the game window could not be reached during the line-up ({})", reason).c_str());
        return;
    }
    const Monitors monitors = Collect(window->hwnd);
    const auto     choice   = monitor_row::Choose(monitors.list, monitors.gameAt, g_columns);
    if (choice.ok && choice.moves.empty())
    {
        platform::Log(L"window span: the desktop reads as the row after {} tick(s)", g_stepTicks);
        if (!PlaceOnRow(*window, choice, g_columns, L"lined up"))
        {
            Leave(L"the reshape after the line-up faulted");
        }
    }
    else if (g_stepTicks > kLineUpTicks)
    {
        platform::LogWarning(L"window span: after the line-up the desktop still does not read as the row ({}) after {} "
                             L"ticks; the desktop now: {}",
                             choice.ok ? std::format(L"{} move(s) still listed", choice.moves.size()) : choice.why,
                             g_stepTicks, DesktopText(monitors));
        Leave(L"the line-up did not take");
    }
}

/// Twice a second while in: the window put back over the row if anything moved it.
void HoldRect() noexcept
{
    if (g_ticks % kHoldTicks != 0 || !IsWindow(g_hwnd))
    {
        return;
    }
    const Rect seen = WindowRect(g_hwnd);
    if (seen == g_target)
    {
        return;
    }
    const wchar_t* why    = L"";
    const auto     window = FindNativeWindow(&why);
    const bool     done   = window && Reshape(window->object, g_target);
    ++g_reapplies;
    platform::Log(L"window span: the window was at {}, not the row; {} (re-apply {})", RectText(seen),
                  done ? L"put back through ReshapeWindow" : L"could NOT be put back", g_reapplies);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    // The vtable is one hand-found data address per build (its slots are read live, not translated one by one).
    const auto engineGlobal = image.Rebase(facts::kGEngine, sizeof(std::uintptr_t));
    const auto vtable       = image.Rebase(facts::kWindowsWindowVTable, sizeof(std::uintptr_t));
    const auto reshape      = vtable ? ReadPointer(*vtable + facts::kWindowsWindowSlotReshapeWindow * 8) : std::nullopt;
    const auto mode         = vtable ? ReadPointer(*vtable + facts::kWindowsWindowSlotGetWindowMode * 8) : std::nullopt;
    const auto handle       = vtable ? ReadPointer(*vtable + facts::kWindowsWindowSlotGetOSWindowHandle * 8)
                                     : std::nullopt;
    // Each function found only where its prologue is (platform::FindFunction logs the one that is not).
    const auto reshapeLive  = platform::FindFunction(image, facts::kWindowsWindowReshapeWindow);
    const auto modeLive     = platform::FindFunction(image, facts::kWindowsWindowGetWindowMode);
    const auto handleLive   = platform::FindFunction(image, facts::kWindowsWindowGetOSWindowHandle);
    const bool codeMatches  = platform::FindFunction(image, facts::kSetWindowTitle).has_value() && reshapeLive &&
                             modeLive && handleLive;
    const bool slotsHold    = codeMatches && reshape && mode && handle && *reshape == *reshapeLive &&
                           *mode == *modeLive && *handle == *handleLive;
    if (!engineGlobal || !vtable || !slotsHold || !codeMatches)
    {
        platform::Log(L"window span: the game window's code is not what the research read (GEngine {}, vtable {}, slots {}, "
                      L"code {}); the span from inside the mod is unavailable (tools/span_window.ps1 remains)",
                      engineGlobal ? L"found" : L"missing", vtable ? L"found" : L"missing", slotsHold ? L"hold" : L"differ",
                      codeMatches ? L"matches" : L"differs");
        return false;
    }
    g_engineGlobal         = *engineGlobal;
    g_vtable               = *vtable;
    g_viewportWindowOffset = facts::kGameViewportWindowOffset;
    g_nativeWindowOffset   = facts::kSWindowNativeWindowOffset;
    g_installed            = true;
    platform::Log(L"window span: ready (Ctrl+F2 or 'window-span on' while player 2 or 3 is in: one monitor per column; "
                  L"Ctrl+F3 or 'monitor-mode on': the three-player monitor mode, three monitors whatever the players; "
                  L"the engine's own window reshape; GameViewportWindow at +{:#x}, NativeWindow at +{:#x})",
                  g_viewportWindowOffset, g_nativeWindowOffset);
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
    if (!In())
    {
        return;
    }
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    const bool     back   = window && Reshape(window->object, g_before);
    const Rect     after  = window ? WindowRect(window->hwnd) : Rect{};
    platform::Log(L"window span: {}: the window {} {}{}; re-applied {} time(s), re-shaped for other columns {} time(s) "
                  L"while in{}",
                  why, back ? L"put back to" : L"could NOT be put back to", RectText(g_before),
                  window ? std::format(L", now {}", RectText(after)) : std::format(L" ({})", reason), g_reapplies,
                  g_reshapes, g_held ? L"; the three-player monitor mode is off" : L"");
    display_align::Restore();  // after the window is back: the engine sees one desktop change, not two at once
    Forget();
}

bool Hold(bool on) noexcept
{
    if (!on)
    {
        if (g_held)
        {
            Leave(L"the three-player monitor mode is switched off");
        }
        return true;
    }
    if (g_held)
    {
        return true;
    }
    if (!g_installed)
    {
        platform::Log(L"window span: not installed; no three-player monitor mode");
        return false;
    }
    const bool spanned = In() ? (g_columns == kModeColumns || Span(kModeColumns, L"the three-player monitor mode"))
                              : Enter(kModeColumns);
    if (!spanned)
    {
        platform::Log(L"window span: the three-player monitor mode needs a row of {} equal monitors (see above); it "
                      L"stays off",
                      kModeColumns);
        return false;
    }
    g_held = true;
    platform::Log(L"window span: the three-player monitor mode is on: {} columns held whatever the players; the "
                  L"mirror views fill the columns no player has",
                  kModeColumns);
    return true;
}

bool Held() noexcept
{
    return g_held;
}

std::int32_t HeldColumns() noexcept
{
    return g_held ? kModeColumns : 0;
}

void Tick(std::int32_t wanted) noexcept
{
    if (!In())
    {
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
    if (wanted >= 2 && wanted != g_columns && wanted <= columns::kMaxColumns)
    {
        const std::int32_t was = g_columns;
        ++g_reshapes;
        if (!Span(wanted, std::format(L"{} columns instead of {}", wanted, was).c_str()))
        {
            Leave(L"the window could not follow the columns");
        }
        return;
    }
    HoldRect();
    if (g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"window span: {}", Reading());
    }
}

std::wstring Reading()
{
    if (!g_installed)
    {
        return L"unavailable";
    }
    if (!In())
    {
        return L"out";
    }
    return std::format(L"{}{}: {} columns, the row has {} monitor(s), the window {} (the row {}), re-applies {}, "
                       L"re-shapes {}{}",
                       g_step == Step::LiningUp ? L"lining up" : L"in", g_held ? L" (the three-player monitor mode)" : L"",
                       g_columns, g_rowMonitors, IsWindow(g_hwnd) ? RectText(WindowRect(g_hwnd)) : std::wstring(L"?"),
                       RectText(g_target), g_reapplies, g_reshapes,
                       display_align::Moved() ? std::format(L"; lined up: {}", display_align::Reading()) : std::wstring());
}

}  // namespace lss::views::window_span
