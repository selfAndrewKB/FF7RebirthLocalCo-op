#include "views/window_span.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/unreal_access.h"
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

constexpr std::int32_t kModeFullscreen         = 0;  // EWindowMode::Fullscreen
constexpr std::int32_t kModeWindowedFullscreen = 1;
constexpr std::int32_t kModeWindowed           = 2;
constexpr std::uint32_t kHoldTicks             = 30;   // the rect check, twice a second at 60
constexpr std::uint32_t kStatusTicks           = 300;

bool           g_installed = false;
std::uintptr_t g_engineGlobal = 0;   // GEngine, live
std::uintptr_t g_vtable       = 0;   // FWindowsWindow's vtable, live
std::uintptr_t g_viewportWindowOffset = 0;  // UGameEngine::GameViewportWindow
std::uintptr_t g_nativeWindowOffset   = 0;  // SWindow::NativeWindow

bool          g_in = false;
void*         g_native = nullptr;  // the native window while in (re-read before each call)
HWND          g_hwnd   = nullptr;
Rect          g_before;            // the window rect before the span
Rect          g_target;
std::int32_t  g_columns   = 0;
std::uint32_t g_ticks     = 0;
std::uint32_t g_reapplies = 0;

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

/// Every monitor with its effective DPI, and the index of the one holding `hwnd`.
struct Monitors
{
    std::vector<monitor_row::Monitor> list;
    std::vector<HMONITOR>             handles;
};

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param) noexcept
{
    auto*        out = reinterpret_cast<Monitors*>(param);
    MONITORINFO  info{};
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
    out->list.push_back({{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom}, dpiX});
    out->handles.push_back(handle);
    return TRUE;
}

bool Reshape(void* native, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), native, rect.left,
                                 rect.top, rect.Width(), rect.Height());
}

void Forget() noexcept
{
    g_in        = false;
    g_native    = nullptr;
    g_hwnd      = nullptr;
    g_columns   = 0;
    g_ticks     = 0;
    g_reapplies = 0;
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
    platform::Log(L"window span: ready ('window-span on' or Ctrl+F2 while player 2 is in: the game window across its row of "
                  L"monitors through the engine's own window reshape; GameViewportWindow at +{:#x}, NativeWindow at +{:#x})",
                  g_viewportWindowOffset, g_nativeWindowOffset);
    return true;
}

bool Installed() noexcept
{
    return g_installed;
}

bool In() noexcept
{
    return g_in;
}

bool Enter(std::int32_t columns) noexcept
{
    if (!g_installed)
    {
        platform::Log(L"window span: not installed");
        return false;
    }
    if (g_in)
    {
        platform::Log(L"window span: already in ({} columns)", g_columns);
        return false;
    }
    if (columns < 2)
    {
        platform::Log(L"window span: {} local player(s); the span needs at least two (one column per seated player)", columns);
        return false;
    }
    const wchar_t* why    = L"";
    const auto     window = FindNativeWindow(&why);
    if (!window)
    {
        platform::Log(L"window span: the game window could not be reached: {}", why);
        return false;
    }
    if (window->mode == kModeFullscreen)
    {
        platform::Log(L"window span: the game is in exclusive fullscreen; set it to windowed fullscreen (borderless) first");
        return false;
    }

    Monitors monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    const HMONITOR own    = MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST);
    std::size_t    gameAt = monitors.handles.size();
    for (std::size_t i = 0; i < monitors.handles.size(); ++i)
    {
        if (monitors.handles[i] == own)
        {
            gameAt = i;
        }
    }
    const auto choice = monitor_row::Choose(monitors.list, gameAt, columns);
    if (!choice.ok)
    {
        platform::Log(L"window span: refused: {} ({} monitor(s) listed; the mod changes no display mode: arrange equal "
                      L"monitors in a row at 100 % scaling, or use tools/display.ps1 on the bench)",
                      choice.why, monitors.list.size());
        return false;
    }

    const Rect before = WindowRect(window->hwnd);
    if (!Reshape(window->object, choice.span))
    {
        platform::Log(L"window span: ReshapeWindow faulted; nothing changed");
        return false;
    }
    const Rect after = WindowRect(window->hwnd);
    g_in        = true;
    g_native    = window->object;
    g_hwnd      = window->hwnd;
    g_before    = before;
    g_target    = choice.span;
    g_columns   = columns;
    g_ticks     = 0;
    g_reapplies = 0;
    platform::Log(L"window span: in: {} columns over the row of {} equal monitor(s) ({} mode{}); the engine's "
                  L"ReshapeWindow({}, {}, {}, {}); the window was ({}, {}, {}, {}), is now ({}, {}, {}, {}){}",
                  columns, choice.rowMonitors, ModeName(window->mode),
                  window->mode == kModeWindowed ? L", so the OS border stays around the client area" : L"",
                  choice.span.left, choice.span.top, choice.span.Width(), choice.span.Height(), before.left, before.top,
                  before.right, before.bottom, after.left, after.top, after.right, after.bottom,
                  after == choice.span || window->mode == kModeWindowed ? L"" : L" (NOT the row yet; held from the tick)");
    return true;
}

void Leave(const wchar_t* why) noexcept
{
    if (!g_in)
    {
        return;
    }
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    const bool     back   = window && Reshape(window->object, g_before);
    const Rect     after  = window ? WindowRect(window->hwnd) : Rect{};
    platform::Log(L"window span: {}: the window {} ({}, {}, {}, {}){}; re-applied {} time(s) while in", why,
                  back ? L"put back to" : L"could NOT be put back to", g_before.left, g_before.top, g_before.right,
                  g_before.bottom, window ? std::format(L", now ({}, {}, {}, {})", after.left, after.top, after.right, after.bottom)
                                          : std::format(L" ({})", reason),
                  g_reapplies);
    Forget();
}

void Tick(bool playerTwoIn) noexcept
{
    if (!g_in)
    {
        return;
    }
    if (!playerTwoIn)
    {
        Leave(L"player 2 is out");
        return;
    }
    ++g_ticks;
    if (g_ticks % kHoldTicks == 0 && IsWindow(g_hwnd))
    {
        const Rect seen = WindowRect(g_hwnd);
        if (seen != g_target)
        {
            const wchar_t* why    = L"";
            const auto     window = FindNativeWindow(&why);
            const bool     done   = window && Reshape(window->object, g_target);
            ++g_reapplies;
            platform::Log(L"window span: the window was at ({}, {}, {}, {}), not the row; {} (re-apply {})", seen.left, seen.top,
                          seen.right, seen.bottom, done ? L"put back through ReshapeWindow" : L"could NOT be put back",
                          g_reapplies);
        }
    }
    if (g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        const Rect seen = WindowRect(g_hwnd);
        platform::LogDebug(L"window span: {} columns, window ({}, {}, {}, {}), re-applies {}", g_columns, seen.left, seen.top,
                           seen.right, seen.bottom, g_reapplies);
    }
}

}  // namespace lss::views::window_span
