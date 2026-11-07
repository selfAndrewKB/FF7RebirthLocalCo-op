#include "hud/view_window.h"

#include "facts/hud.h"
#include "hud/passes/pass_opacity.h"
#include "platform/guarded_read.h"
#include "platform/log.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <Unreal/UObjectGlobals.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::view_window {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace unreal = players::unreal;
namespace weak   = players::weak_object;

static_assert(kViews == players::kMaxPlayers, "a window per player");

/// The window's flags in the game's enum (EEndMenuMessageWindowFlags): PauseMenu adds it to the overlay above the
/// HUD (z 0x7C6), not under it (z 0).
constexpr std::uint8_t kFlagsAboveHud = 2;

/// The reflected calls and their parameters, found at the first show (reflection is up by then).
struct Calls
{
    bool                 looked = false;
    unreal::StaticCall   show, hide, closed;
    UFunction*           pop        = nullptr;  // UEndUserWidget::PopFromFocusStack()
    std::int32_t showContext = -1, showText = -1, showFlags = -1, showResult = -1;
    std::int32_t hideWidget = -1, closedWidget = -1, closedResult = -1;
};

/// A window the mod opened, by the view that shows it. The weak value confirms it is still that object.
struct Window
{
    int           view   = -1;
    void*         object = nullptr;
    std::uint64_t pair   = 0;
    std::wstring  text;
};

// Game thread.
Calls                     g;
std::array<Window, kViews> g_open;     // each view's open window (view -1: none)
std::vector<Window>       g_closing;  // closed ones still playing Out (each still shown in its own view only)
HiddenSet                 g_hidden;   // the windows made transparent for the pass
std::uint64_t             g_opened = 0, g_changes = 0, g_failed = 0, g_gameClosed = 0;
bool                      g_failLogged = false;

std::int32_t Offset(UFunction* function, const wchar_t* name, std::int32_t size) noexcept
{
    const auto offset = function != nullptr ? unreal::ParamOffset(function, name, size) : std::nullopt;
    return offset ? *offset : -1;
}

UFunction* Find(const wchar_t* path) noexcept
{
    try
    {
        return RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    }
    catch (...)
    {
        return nullptr;
    }
}

/// Every call and parameter by name; false (logged once) if any is missing or shaped otherwise.
bool LookUp()
{
    if (g.looked)
    {
        return g.show.function != nullptr;
    }
    g.looked                  = true;
    constexpr auto kString    = static_cast<std::int32_t>(sizeof(RC::Unreal::FString));
    constexpr auto kPointer   = static_cast<std::int32_t>(sizeof(void*));
    constexpr wchar_t kApi[]  = L"/Script/EndGame.EndMenuAPI";
    Calls c;
    c.looked       = true;
    c.show         = unreal::FindStatic(kApi, L"/Script/EndGame.EndMenuAPI:ShowMessageWindow");
    c.hide         = unreal::FindStatic(kApi, L"/Script/EndGame.EndMenuAPI:HideMessageWindow");
    c.closed       = unreal::FindStatic(kApi, L"/Script/EndGame.EndMenuAPI:IsMessageWindowClosed");
    c.pop          = Find(L"/Script/EndGame.EndUserWidget:PopFromFocusStack");
    c.showContext  = Offset(c.show.function, L"WorldContextObject", kPointer);
    c.showText     = Offset(c.show.function, L"TextId", kString);
    c.showFlags    = Offset(c.show.function, L"Flags", 1);
    c.showResult   = Offset(c.show.function, L"ReturnValue", kPointer);
    c.hideWidget   = Offset(c.hide.function, L"Widget", kPointer);
    c.closedWidget = Offset(c.closed.function, L"Widget", kPointer);
    c.closedResult = Offset(c.closed.function, L"ReturnValue", 1);
    const bool all = c.show.defaultObject != nullptr && c.hide.defaultObject != nullptr && c.closed.defaultObject != nullptr &&
                     c.pop != nullptr && c.showContext >= 0 && c.showText >= 0 && c.showFlags >= 0 && c.showResult >= 0 &&
                     c.hideWidget >= 0 && c.closedWidget >= 0 && c.closedResult >= 0;
    if (!all)
    {
        const auto missing = [](bool ok, const wchar_t* what) { return ok ? std::wstring() : std::wstring(L" ") + what; };
        platform::LogWarning(L"view window: the game's message window calls are not what the research read (missing:{}{}{}{}"
                             L"{}{}{}{}{}{}{}); the player windows are drawn in the engine's own font instead",
                             missing(c.show.defaultObject != nullptr, L"ShowMessageWindow"),
                             missing(c.hide.defaultObject != nullptr, L"HideMessageWindow"),
                             missing(c.closed.defaultObject != nullptr, L"IsMessageWindowClosed"),
                             missing(c.pop != nullptr, L"PopFromFocusStack"),
                             missing(c.showContext >= 0, L"show.WorldContextObject"), missing(c.showText >= 0, L"show.TextId"),
                             missing(c.showFlags >= 0, L"show.Flags"), missing(c.showResult >= 0, L"show.ReturnValue"),
                             missing(c.hideWidget >= 0, L"hide.Widget"), missing(c.closedWidget >= 0, L"closed.Widget"),
                             missing(c.closedResult >= 0, L"closed.ReturnValue"));
        g = Calls{.looked = true};
        return false;
    }
    g = c;
    platform::Log(L"view window: the game's message window can be shown in one player's view");
    return true;
}

/// The window is still the one recorded and the game has not closed it.
bool Alive(const Window& w) noexcept
{
    if (w.object == nullptr || !weak::StillNames(w.pair, w.object))
    {
        return false;
    }
    alignas(16) std::uint8_t frame[32]{};
    std::memcpy(frame + g.closedWidget, &w.object, sizeof(void*));
    return unreal::GuardedProcessEvent(g.closed.defaultObject, g.closed.function, frame) && frame[g.closedResult] == 0;
}

/// The game's script call for one window, its focus layer popped at once.
void* OpenWindow(int view, const std::wstring& text)
{
    UObject* const context = unreal::GameInstance();
    void*          w       = nullptr;
    if (context == nullptr ||
        !unreal::CallWithString(
            g.show.defaultObject, g.show.function, g.showText, text.c_str(),
            [&](std::uint8_t* frame) {
                std::memcpy(frame + g.showContext, &context, sizeof(void*));
                frame[g.showFlags] = kFlagsAboveHud;
            },
            [&](const std::uint8_t* frame) { std::memcpy(&w, frame + g.showResult, sizeof(w)); }) ||
        w == nullptr)
    {
        return nullptr;
    }
    if (!unreal::GuardedProcessEvent(static_cast<UObject*>(w), g.pop, nullptr))
    {
        platform::LogWarning(L"view window: {}'s window opened but its focus pop faulted; closed again", players::SeatName(view));
        const std::uint8_t remove = 1;
        platform::WriteGuarded(static_cast<std::uint8_t*>(w) + facts::kMessageWindowRemoveAfterOut, &remove, 1);
        alignas(16) std::uint8_t frame[16]{};
        std::memcpy(frame + g.hideWidget, &w, sizeof(void*));
        unreal::GuardedProcessEvent(g.hide.defaultObject, g.hide.function, frame);
        return nullptr;
    }
    ++g_opened;
    platform::Log(L"view window: opened in {}'s view (the game's message window, its focus popped)", players::SeatName(view));
    return w;
}

/// The windows gone from the closing list (their objects collected).
void PruneClosing() noexcept
{
    std::erase_if(g_closing, [](const Window& c) { return !weak::StillNames(c.pair, c.object); });
}

/// w kept to its own view while it plays Out (a window the mod closed, or one the game closed under it), then
/// forgotten here.
void KeepWhileClosing(Window& w)
{
    PruneClosing();
    if (w.object != nullptr && weak::StillNames(w.pair, w.object))
    {
        g_closing.push_back(w);
    }
    w = Window{};
}

/// The game's close, the window leaving the overlay when its Out animation ends.
void CloseWindow(Window& w)
{
    if (Alive(w))
    {
        const std::uint8_t remove = 1;
        platform::WriteGuarded(static_cast<std::uint8_t*>(w.object) + facts::kMessageWindowRemoveAfterOut, &remove, 1);
        alignas(16) std::uint8_t frame[16]{};
        std::memcpy(frame + g.hideWidget, &w.object, sizeof(void*));
        unreal::GuardedProcessEvent(g.hide.defaultObject, g.hide.function, frame);
    }
    KeepWhileClosing(w);
}

#if LSS_DEV
std::wstring Reading()
{
    std::wstring open;
    for (const Window& w : g_open)
    {
        if (w.view >= 0)
        {
            open += std::format(L"{}{}", open.empty() ? L"" : L", ", players::SeatName(w.view));
        }
    }
    return std::format(L"{}; open in: {}; closing {}; opened {}, reopened for a new text {}, failed {}, closed by the game {}",
                       g.show.function != nullptr ? L"on" : g.looked ? L"unavailable" : L"not looked up yet",
                       open.empty() ? L"none" : open, g_closing.size(), g_opened, g_changes, g_failed, g_gameClosed);
}
#endif

}  // namespace

bool Show(int view, const std::wstring& markup) noexcept
try
{
    if (view < 0 || view >= kViews)
    {
        return false;
    }
    Window& w = g_open[static_cast<std::size_t>(view)];
    if (!weak::Known() || !LookUp())
    {
        CloseWindow(w);  // none can be shown: the fallback draws instead, never over an old window
        return false;
    }
    if (w.view >= 0 && !Alive(w))
    {
        ++g_gameClosed;  // the game closed it (a reset, or the object collected): opened again below
        KeepWhileClosing(w);
    }
    bool reopen = false;
    if (w.view >= 0)
    {
        if (w.text == markup)
        {
            return true;
        }
        CloseWindow(w);  // another text: the game's window again, through the same call
        reopen = true;
    }
    void* const object = OpenWindow(view, markup);
    if (object == nullptr)
    {
        ++g_failed;
        if (!g_failLogged)
        {
            g_failLogged = true;
            platform::Log(L"view window: no game window could be opened in {}'s view; the engine's font draws it",
                          players::SeatName(view));
        }
        return false;
    }
    w = Window{view, object, weak::WeakOf(object), markup};
    g_changes += reopen ? 1 : 0;
    return true;
}
catch (...)
{
    return false;
}

void Close(int view) noexcept
try
{
    if (view >= 0 && view < kViews && g_open[static_cast<std::size_t>(view)].view >= 0)
    {
        CloseWindow(g_open[static_cast<std::size_t>(view)]);
    }
}
catch (...)
{
}

void CloseAll() noexcept
{
    for (int view = 0; view < kViews; ++view)
    {
        Close(view);
    }
}

unsigned long long ClosedByGame() noexcept
{
    return g_gameClosed;
}

void ShowOnly(int seat) noexcept
{
    // The pass before's hides back first: a window hidden there is its own player's in this pass.
    g_hidden.RestoreAll();
    PruneClosing();
    // Closing windows still play Out: kept to their own views.
    for (const Window& c : g_closing)
    {
        if (c.view != seat)
        {
            g_hidden.Hide(c.object, c.pair);
        }
    }
    for (const Window& w : g_open)
    {
        if (w.view >= 0 && w.view != seat && weak::StillNames(w.pair, w.object))
        {
            g_hidden.Hide(w.object, w.pair);
        }
    }
}

void RestoreAll() noexcept
{
    g_hidden.RestoreAll();
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"views.window",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's message windows the mod shows in the players' views (the "
                                L"player windows)",
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

}  // namespace lss::hud::view_window
