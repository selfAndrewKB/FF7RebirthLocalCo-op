#include "menu/coop_page_open.h"

#include "facts/menu.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/game_ui.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/weak_object.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::menu::coop_page_open {

namespace {

namespace rules = coop_menu_rules;
namespace weak  = players::weak_object;

using game_ui::At;

using ShowTopFn    = void* (*)(void* mgr, bool restoreCursor);
using KeyDownFn    = std::uintptr_t (*)(void* page, void* a, void* b, void* c);
using OpenFn       = void (*)(void* page);
using TakeWidgetFn = void* (*)(void* widget, void* out);
using PopLayerFn   = void (*)(void* ui, void* taken);
using ObjectFn     = void* (*)(void* object);  // a window's Hide

// The guide while the page is open: the page's own keys (the first page's guide shows whatever has focus).
constexpr const wchar_t* kGuide = L"<button=decide> Choose  <button=cancel> Back";

/// The game's functions, found at install.
struct Game
{
    ShowTopFn      showTop      = nullptr;
    OpenFn         open         = nullptr;
    TakeWidgetFn   takeWidget   = nullptr;
    PopLayerFn     popLayer     = nullptr;
    std::uintptr_t headings     = 0;  // the heading table (live)
    std::uint32_t  headingCount = 0;
};

// Set at install and on the first tick, then only read.
Game                  g;
platform::VTableEntry g_keyDown;
/// The page's categories' entries of the game's heading table, swapped for their labels while the page is open (the
/// game's own Options page reads the same table).
std::array<platform::VTableEntry, rules::kCategoryKeys.size()> g_headings;
std::array<bool, rules::kCategoryKeys.size()>                  g_headingSet{};  // game thread: the entry holds the label

// Game thread.
void*         g_mgr     = nullptr;  // the main menu manager while open
std::uint64_t g_topWeak = 0;        // the hidden first page (a weak value)
std::wstring  g_savedTitle, g_savedGuide;
std::uint64_t g_opens = 0;

/// The heading table's entries for the page's categories: their labels while the page is open, the game's after.
void SwapHeadings(bool labels) noexcept
{
    const auto& page = coop_page::Categories();
    for (std::size_t i = 0; i < page.size() && i < g_headings.size(); ++i)
    {
        const std::uint8_t key = page[i].key;
        if (key >= g.headingCount)
        {
            continue;  // past the table: the game shows no heading there
        }
        if (!labels)
        {
            if (g_headingSet[i])
            {
                g_headings[i].Restore();
                g_headingSet[i] = false;
            }
            continue;
        }
        const std::uintptr_t at  = g.headings + 8 * static_cast<std::uintptr_t>(key);
        std::uintptr_t       was = 0;
        if (g_headingSet[i])
        {
            continue;
        }
        g_headingSet[i] =
            platform::Read(at, was) && g_headings[i].Replace(at, was, reinterpret_cast<std::uintptr_t>(page[i].text.label));
        if (!g_headingSet[i])
        {
            platform::LogWarning(L"Co-op page: the heading of {} could not be set; it shows the game's", page[i].text.label);
        }
    }
}

/// The first page's title and guide back as they were.
void RestoreTop() noexcept
{
    void* const top = g_topWeak != 0 ? weak::ObjectOf(g_topWeak) : nullptr;
    if (top == nullptr)
    {
        return;
    }
    if (!game_ui::AssignString(At(top, facts::kWindowTitle), g_savedTitle) ||
        !game_ui::AssignString(At(top, facts::kTopGuide), g_savedGuide))
    {
        platform::LogWarning(L"Co-op page: the main menu's title or guide could not be put back");
    }
}

/// The page closed (by its own Back, or gone): the first page back, as after any of the game's windows.
void Closed(const wchar_t* how) noexcept
{
    coop_page::SetOpen(false);
    SwapHeadings(false);
    RestoreTop();
    void*      shown = nullptr;
    const bool ok    = g_mgr != nullptr && platform::CallGuarded(g.showTop, shown, g_mgr, true) && shown != nullptr;
    platform::Log(L"Co-op page: closed ({}); {}", how,
                  ok ? L"the main menu is back" : L"the main menu could not be shown again (it closed)");
    g_mgr = nullptr;
}

bool PageClosing(const void* page) noexcept;

/// The page taken down the way its own no-animation Back does (its focus layer popped, out of the overlay), for a main
/// menu closed under it.
void ForceClose(void* page, const wchar_t* why) noexcept
{
    coop_page::SetOpen(false);
    SwapHeadings(false);
    if (page != nullptr)
    {
        // Its layer popped unless its own Back already did (closing or closed).
        alignas(16) std::uint8_t taken[16]{};
        void*                    ref = nullptr;
        if (!PageClosing(page) && platform::CallGuarded(g.takeWidget, ref, page, static_cast<void*>(taken)) && ref != nullptr)
        {
            platform::CallGuarded(g.popLayer, game_ui::System(), ref);
        }
        coop_page::Abandon(page);
    }
    RestoreTop();
    g_mgr = nullptr;
    platform::Log(L"Co-op page: closed with the main menu ({}); its layer popped and the page taken out", why);
}

/// The page's state says it is closing or closed.
bool PageClosing(const void* page) noexcept
{
    std::uint8_t state = 0xFF;
    return platform::Read(static_cast<const std::uint8_t*>(page) + facts::kOptionsState, state) &&
           (state == facts::kOptionsStateClosing || state == 0);
}

/// The page's key handler (vtable slot): the game's, then the first page back if Back closed the mod's page.
std::uintptr_t KeyDown(void* page, void* a, void* b, void* c)
{
    const std::uintptr_t reply = reinterpret_cast<KeyDownFn>(g_keyDown.Original())(page, a, b, c);
    if (coop_page::IsOpen() && coop_page::IsPage(page) && PageClosing(page))
    {
        Closed(L"Back");
    }
    return reply;
}

/// The menu tutorial holds the opener (the states in which the game's opener opens nothing).
bool TutorialBusy() noexcept
{
    std::uint8_t state = 0;
    void* const  t     = game_ui::Manager(facts::kUiMenuTutorialSlot);
    return t != nullptr && platform::Read(At(t, facts::kMenuTutorialState), state) &&
           std::find(facts::kMenuTutorialBusy.begin(), facts::kMenuTutorialBusy.end(), state) != facts::kMenuTutorialBusy.end();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f            = facts;
    const auto showTop     = platform::FindFunction(image, f::kShowTopWindow);
    const auto open        = platform::FindFunction(image, f::kOptionsOpen);
    const auto takeWidget  = platform::FindFunction(image, f::kTakeWidget);
    const auto popLayer    = platform::FindFunction(image, f::kPopFocusLayer);
    const auto count       = static_cast<std::uint32_t>(f::kOptionsHeadingCount);
    const auto headings    = image.Rebase(f::kOptionsHeadings, 8 * count);
    if (!showTop || !open || !takeWidget || !popLayer || !headings)
    {
        return false;
    }
    g.showTop      = reinterpret_cast<ShowTopFn>(*showTop);
    g.open         = reinterpret_cast<OpenFn>(*open);
    g.takeWidget   = reinterpret_cast<TakeWidgetFn>(*takeWidget);
    g.popLayer     = reinterpret_cast<PopLayerFn>(*popLayer);
    g.headings     = *headings;
    g.headingCount = count;
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    return platform::ReplaceSlot(image, facts::kOptionsKeyDown, reinterpret_cast<std::uintptr_t>(&KeyDown), g_keyDown);
}

void Unplace() noexcept
{
    SwapHeadings(false);
    g_keyDown.Restore();
}

void Remove() noexcept
{
    if (coop_page::IsOpen())
    {
        coop_page::SetOpen(false);
        platform::Log(L"Co-op page: the mod unloads with the page open; the page is left to the game");
    }
}

void Open(void* mgr, OpenWindowFn openWindow) noexcept
try
{
    if (coop_page::IsOpen() || coop_page::Failed())
    {
        return;
    }
    if (TutorialBusy())
    {
        platform::LogDebug(L"Co-op page: the menu tutorial is busy; not opened, as the game opens no window then");
        return;
    }
    platform::CallGuarded(openWindow, mgr, facts::kOpenWindowPreludeOnly);
    void* const  top        = game_ui::WeakAt(mgr, facts::kMainMenuMgrCurrent);
    std::uint8_t topCurrent = 0;
    if (!game_ui::IsTopWindow(top) || !platform::Read(At(mgr, facts::kMainMenuMgrTopCurrent), topCurrent) || topCurrent != 1)
    {
        platform::LogWarning(L"Co-op page: the main menu's current window is not its first page; not opened");
        return;
    }
    std::int32_t z = 0;
    platform::Read(At(top, facts::kWindowZOrder), z);
    void* const page = coop_page::Ready(top, z, std::uint8_t{1});
    if (page == nullptr)
    {
        return;
    }
    const std::uint64_t topWeak = weak::WeakOf(top);  // without it the first page's title could not be put back
    const auto          hide    = reinterpret_cast<ObjectFn>(
        platform::ReadPointer(platform::ReadPointer(top, 0), static_cast<std::uint32_t>(facts::kWindowHideSlot)));
    void* ignored = nullptr;
    if (topWeak == 0 || hide == nullptr || !platform::CallGuarded(hide, ignored, top))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be hidden; not opened");
        coop_page::Abandon(page);
        return;
    }
    const std::uint8_t hidden = 0;
    platform::WriteGuarded(At(top, facts::kWindowVisible), &hidden, 1);
    g_topWeak    = topWeak;
    g_savedTitle = game_ui::ReadString(At(top, facts::kWindowTitle));
    g_savedGuide = game_ui::ReadString(At(top, facts::kTopGuide));
    game_ui::AssignString(At(top, facts::kWindowTitle), rules::kTitle);
    game_ui::AssignString(At(top, facts::kTopGuide), kGuide);
    SwapHeadings(true);
    g_mgr = mgr;
    coop_page::SetOpen(true);
    if (!platform::CallGuarded(g.open, page))
    {
        platform::LogWarning(L"Co-op page: opening the page faulted; the main menu comes back");
        coop_page::Abandon(page);
        Closed(L"its opening faulted");
        return;
    }
    ++g_opens;
    platform::Log(L"Co-op page: opened ({} categories, {} rows; the main menu's first page hidden, its title Co-op)",
                  coop_page::Categories().size(), [] {
                      std::size_t rows = 0;
                      for (const auto& category : coop_page::Categories())
                      {
                          rows += category.options.size();
                      }
                      return rows;
                  }());
}
catch (...)
{
    platform::LogWarning(L"Co-op page: opening raised an exception");
}

void Tick() noexcept
{
    if (!coop_page::IsOpen())
    {
        return;
    }
    // A main menu closed under the page: take it down.
    void* const  page     = coop_page::Page();
    void* const  mgr      = game_ui::Manager(facts::kUiMainMenuSlot);
    std::uint8_t menuOpen = 0;
    const bool   menuUp   = mgr == g_mgr && platform::Read(At(mgr, facts::kMainMenuMgrOpen), menuOpen) &&
                        menuOpen == facts::kMainMenuOpen && game_ui::WeakAt(mgr, facts::kMainMenuMgrMenu) != nullptr;
    if (page == nullptr)
    {
        platform::LogWarning(L"Co-op page: the page is gone while open");
        if (menuUp)
        {
            Closed(L"the page is gone");
        }
        else
        {
            // The first page is shown again only while the main menu is open, as the game shows it.
            coop_page::SetOpen(false);
            SwapHeadings(false);
            RestoreTop();
            g_mgr = nullptr;
            platform::Log(L"Co-op page: the page is gone with the main menu closing; its title and guide put back");
        }
        return;
    }
    if (!menuUp)
    {
        ForceClose(page, L"the main menu closed");
        return;
    }
    if (PageClosing(page))
    {
        Closed(L"seen by the tick");
    }
}

std::uint64_t Opens() noexcept
{
    return g_opens;
}

}  // namespace lss::menu::coop_page_open
