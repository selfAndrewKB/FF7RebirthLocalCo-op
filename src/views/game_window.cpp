#include "views/game_window.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
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
    const auto engineGlobal = players::game_globals::Address(players::game_globals::Global::Engine);
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
    g_engineGlobal         = engineGlobal;
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
    window.object  = reinterpret_cast<void*>(*native);
    window.swindow = reinterpret_cast<void*>(*swindow);
    void* handle   = nullptr;
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

Rect ClientRect(HWND hwnd) noexcept
{
    RECT  client{};
    POINT origin{};
    GetClientRect(hwnd, &client);
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

bool Reshape(void* window, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), window, rect.left, rect.top,
                                 rect.Width(), rect.Height());
}

namespace {

/// One of the limit's TOptional<float>s as the engine keeps it: the value and its IsSet byte.
struct Optional
{
    float        value = 0.0f;
    std::uint8_t set   = 0;
};

/// The limit a window could have: an IsSet byte of 0 or 1, a set value finite and within a desktop's reach.
bool Plausible(const Optional& o) noexcept
{
    constexpr float kWidest = 65536.0f;
    return o.set <= 1 && (o.set == 0 || (std::isfinite(o.value) && o.value > 0.0f && o.value < kWidest));
}

std::uint8_t* LimitField(const Window& window, std::uint32_t which) noexcept
{
    return static_cast<std::uint8_t*>(window.swindow) + facts::kSWindowSizeLimitsOffset + which;
}

bool ReadOptional(const Window& window, std::uint32_t which, Optional& out) noexcept
{
    return platform::Read(LimitField(window, which), out.value) &&
           platform::Read(LimitField(window, which + facts::kOptionalIsSet), out.set) && Plausible(out);
}

bool WriteOptional(const Window& window, std::uint32_t which, const Optional& value) noexcept
{
    return platform::WriteGuarded(LimitField(window, which), &value.value, sizeof(value.value)) &&
           platform::WriteGuarded(LimitField(window, which + facts::kOptionalIsSet), &value.set, sizeof(value.set));
}

}  // namespace

bool RaiseSizeLimit(const Window& window, std::int32_t width, std::int32_t height, std::optional<SizeLimit>& before) noexcept
{
    before.reset();
    Optional maxWidth, maxHeight;
    if (window.swindow == nullptr || !ReadOptional(window, facts::kSizeLimitsMaxWidth, maxWidth) ||
        !ReadOptional(window, facts::kSizeLimitsMaxHeight, maxHeight))
    {
        return false;
    }
    if (maxWidth.set != maxHeight.set)
    {
        return false;  // the engine sets both or neither
    }
    if (maxWidth.set == 0 || (maxWidth.value >= static_cast<float>(width) && maxHeight.value >= static_cast<float>(height)))
    {
        return true;  // no limit, or one the row fits under
    }
    const Optional newWidth{std::max(maxWidth.value, static_cast<float>(width)), 1};
    const Optional newHeight{std::max(maxHeight.value, static_cast<float>(height)), 1};
    if (!WriteOptional(window, facts::kSizeLimitsMaxWidth, newWidth) || !WriteOptional(window, facts::kSizeLimitsMaxHeight, newHeight))
    {
        // A half-raised limit is put back at once (the write that faulted is the one tried again; a second fault leaves
        // the limit as it is, which only ever widens the window's allowance).
        (void)WriteOptional(window, facts::kSizeLimitsMaxWidth, maxWidth);
        (void)WriteOptional(window, facts::kSizeLimitsMaxHeight, maxHeight);
        return false;
    }
    before = SizeLimit{true, maxWidth.value, maxHeight.value};
    return true;
}

bool RestoreSizeLimit(const Window& window, const SizeLimit& before) noexcept
{
    const Optional width{before.width, before.set ? std::uint8_t{1} : std::uint8_t{0}};
    const Optional height{before.height, before.set ? std::uint8_t{1} : std::uint8_t{0}};
    return window.swindow != nullptr && WriteOptional(window, facts::kSizeLimitsMaxWidth, width) &&
           WriteOptional(window, facts::kSizeLimitsMaxHeight, height);
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
