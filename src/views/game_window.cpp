#include "views/game_window.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <format>

namespace lss::views::game_window {

namespace {

// FGenericWindow's virtuals the module calls, by their stock slot (the research confirmed the shipped table keeps
// the order).
using ReshapeWindowFn     = void (*)(void* self, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
using GetWindowModeFn     = std::int32_t (*)(void* self);
using GetOSWindowHandleFn = void* (*)(void* self);

std::uintptr_t g_engineGlobal         = 0;  // GEngine, live
std::uintptr_t g_vtable               = 0;  // FWindowsWindow's vtable, live
std::uintptr_t g_viewportWindowOffset = 0;  // UGameEngine::GameViewportWindow
std::uintptr_t g_nativeWindowOffset   = 0;  // SWindow::NativeWindow

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
    platform::Log(L"window span: the game window's chain checked (GameViewportWindow at +{:#x}, NativeWindow at +{:#x})",
                  g_viewportWindowOffset, g_nativeWindowOffset);
    return true;
}

/// GEngine -> GameViewportWindow (TWeakPtr<SWindow>: the object pointer first) -> SWindow::NativeWindow (TSharedPtr: the
/// object pointer first). Every read guarded; the vtable must be the one the research found, and its GetOSWindowHandle
/// must name a top-level window of this process.
std::optional<Window> Find(const wchar_t** why) noexcept
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
    Window window;
    window.object = reinterpret_cast<void*>(*native);
    void* handle  = nullptr;
    if (!platform::CallGuarded(Slot<GetOSWindowHandleFn>(facts::kWindowsWindowSlotGetOSWindowHandle), handle, window.object))
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
    if (!platform::CallGuarded(Slot<GetWindowModeFn>(facts::kWindowsWindowSlotGetWindowMode), window.mode, window.object))
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

bool Reshape(void* window, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), window, rect.left, rect.top,
                                 rect.Width(), rect.Height());
}

std::wstring RectText(const Rect& r)
{
    return std::format(L"({}, {}, {}, {}) {}x{}", r.left, r.top, r.right, r.bottom, r.Width(), r.Height());
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

}  // namespace lss::views::game_window
