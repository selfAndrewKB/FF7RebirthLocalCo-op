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
#include "players/generation.h"
#include "players/unreal_access.h"
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

/// The game's functions, found at install.
struct Game
{
    ShowTopFn      showTop      = nullptr;
    OpenFn         open         = nullptr;
    TakeWidgetFn   takeWidget   = nullptr;
    PopLayerFn     popLayer     = nullptr;
    std::uintptr_t headings     = 0;  // the heading table (live)
    std::uint32_t  headingCount = 0;
    // The first page's guide getter slot, the function it holds and the base window's getter (live; 0 if not found:
    // the page then shows the first page's own guide).
    std::uintptr_t guideSlot = 0, topGuide = 0, windowGuide = 0;
};

// Set at install and on the first tick, then only read.
Game                  g;
platform::VTableEntry g_keyDown;
platform::VTableEntry g_guide;  // game thread: the first page's guide getter while the page is open
/// The page's categories' entries of the game's heading table, swapped for their labels while the page is open (the
/// game's own Options page reads the same table).
std::array<platform::VTableEntry, rules::kCategoryKeys.size()> g_headings;
std::array<bool, rules::kCategoryKeys.size()>                  g_headingSet{};  // game thread: the entry holds the label

using TitleCallFn = void (*)(void* start);  // the start menu's Options open
using ClosedFn    = void (*)(const void* capture);
using SetStickFn  = void (*)(void* input, std::uint8_t on);

/// The title's functions, found at install (menu/title_row).
struct TitleGame
{
    TitleCallFn openOptions = nullptr;
    ClosedFn    closed      = nullptr;
    SetStickFn  setStick    = nullptr;
};

/// What the game's close function for the title holds: the start menu and a weak pointer to it.
struct TitleCapture
{
    void*         start = nullptr;
    std::uint64_t weak  = 0;
};
static_assert(sizeof(TitleCapture) == 16 && offsetof(TitleCapture, weak) == 8, "read raw by the game's close function");

TitleGame      g_title;
std::uintptr_t g_acceptKey = 0;  // the Accept key's FKey global (live; its FName first)

// Game thread.
void*         g_mgr     = nullptr;  // the main menu manager while open
std::uint64_t g_topWeak = 0;        // the hidden first page (a weak value)
void*         g_heldTop = nullptr;  // the hidden first page while the mod holds it from the garbage collector
bool          g_atTitle   = false;  // the page is open over the title menu, not the main menu
std::uint64_t g_titleHost = 0;      // the start menu it opened over (a weak value)
std::wstring  g_savedTitle;
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

void ReleaseTop() noexcept;

/// The hidden first page kept from the garbage collector while the page is open (research/RE-coop-page-guide.md): its
/// Hide takes it out of the overlay and nothing else holds it, yet the main menu keeps it the current window, whose
/// tick empties the title, help and guide every frame once it is collected. Held as AddToRoot holds, and only if no one
/// else rooted it.
void HoldTop(void* top) noexcept
{
    if (g_heldTop != nullptr && g_heldTop != top)
    {
        ReleaseTop();  // an older hold is let go, never lost by an overwrite
    }
    bool rooted = false;
    if (!players::unreal::ReadRootFlag(top, rooted))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be read in the object array; its title, "
                             L"help and guide may go blank at the next garbage collection");
        return;
    }
    if (rooted)
    {
        return;  // someone else's hold: left as found
    }
    if (!players::unreal::SetRootFlag(top, true))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be held; its title, help and guide may "
                             L"go blank at the next garbage collection");
        return;
    }
    g_heldTop = top;
    platform::LogDebug(L"Co-op page: the main menu's hidden first page held from the garbage collection while the page "
                       L"is open");
}

/// The hold let go (the page closed, gone, or the mod unloading): the first page is the game's to keep again. Only on the
/// game thread (the flag shares its word with the collector's own); off it (an unload) the hold is forgotten and the page
/// stays held until the game ends.
void ReleaseTop() noexcept
{
    if (g_heldTop == nullptr)
    {
        return;
    }
    if (!players::generation::OnGameThread())
    {
        g_heldTop = nullptr;
        platform::LogDebug(L"Co-op page: the main menu's first page left held: the unload runs off the game thread");
        return;
    }
    if (!players::unreal::SetRootFlag(g_heldTop, false))
    {
        platform::LogWarning(L"Co-op page: the hold on the main menu's first page could not be let go; it stays until the "
                             L"game ends");
    }
    else
    {
        platform::LogDebug(L"Co-op page: the main menu's first page let go");
    }
    g_heldTop = nullptr;
}

/// The first page's guide getter while the page is open: the base window's, which every other window of the main menu
/// uses, so the guide is the focused row's (hidden over an open gauge or list, back at a cancel), as over the game's own
/// Options page. The first page's own getter back at every close.
void SwapGuide(bool focused) noexcept
{
    if (!focused)
    {
        g_guide.Restore();
        return;
    }
    if (g.guideSlot != 0 && !g_guide.Replace(g.guideSlot, g.topGuide, g.windowGuide))
    {
        platform::LogWarning(L"Co-op page: the first page's guide getter is not the expected one; the page shows the main "
                             L"menu's guide");
    }
}

/// The first page's guide getter and title back as they were.
void RestoreTop() noexcept
{
    SwapGuide(false);
    void* const top = g_topWeak != 0 ? weak::ObjectOf(g_topWeak) : nullptr;
    if (top == nullptr)
    {
        return;
    }
    if (!game_ui::AssignString(At(top, facts::kWindowTitle), g_savedTitle))
    {
        platform::LogWarning(L"Co-op page: the main menu's title could not be put back");
    }
}

bool PageClosing(const void* page) noexcept;

/// The mod's state at a close over the title menu; the game's close function has run with the page's Back.
void ClosedAtTitle(const wchar_t* how) noexcept
{
    coop_page::SetOpen(false);
    SwapHeadings(false);
    g_atTitle   = false;
    g_titleHost = 0;
    platform::Log(L"Co-op page: closed over the title menu ({})", how);
}

/// The page gone or taken down over the title menu, so its Back never ran: what that Back's close function undoes,
/// undone. With the start menu gone too, only the stick's mode is left to put back.
void LeftAtTitle(void* page, const wchar_t* why) noexcept
{
    coop_page::SetOpen(false);
    SwapHeadings(false);
    if (page != nullptr)
    {
        alignas(16) std::uint8_t taken[16]{};
        void*                    ref = nullptr;
        if (!PageClosing(page) && platform::CallGuarded(g.takeWidget, ref, page, static_cast<void*>(taken)) && ref != nullptr)
        {
            platform::CallGuarded(g.popLayer, game_ui::System(), ref);
        }
        coop_page::Abandon(page);
    }
    if (void* const start = g_titleHost != 0 ? weak::ObjectOf(g_titleHost) : nullptr)
    {
        const TitleCapture capture{start, g_titleHost};
        platform::CallGuarded(g_title.closed, static_cast<const void*>(&capture));
    }
    else
    {
        platform::CallGuarded(g_title.setStick, game_ui::Manager(facts::kUiInputSlot), std::uint8_t{0});
    }
    g_atTitle   = false;
    g_titleHost = 0;
    platform::LogWarning(L"Co-op page: left over the title menu ({})", why);
}

/// The page closed (by its own Back, or gone): the first page back, as after any of the game's windows.
void Closed(const wchar_t* how) noexcept
{
    if (g_atTitle)
    {
        ClosedAtTitle(how);
        return;
    }
    coop_page::SetOpen(false);
    SwapHeadings(false);
    RestoreTop();
    void*      shown = nullptr;
    const bool ok    = g_mgr != nullptr && platform::CallGuarded(g.showTop, shown, g_mgr, true) && shown != nullptr;
    ReleaseTop();  // after the show: it finds the first page alive, as the game does when no collection fell in between
    platform::Log(L"Co-op page: closed ({}); {}", how,
                  ok ? L"the main menu is back" : L"the main menu could not be shown again (it closed)");
    g_mgr = nullptr;
}

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
    ReleaseTop();
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

/// Accept (not a repeat) on the mod's page while one of its Range rows is open.
bool RangeAccept(void* page, const void* event) noexcept
{
    std::uint8_t  state = 0, repeat = 1;
    std::uint64_t key = 0, accept = 0;
    return g_acceptKey != 0 && event != nullptr && coop_page::IsOpen() && coop_page::IsPage(page) &&
           platform::Read(At(page, facts::kOptionsState), state) && state == facts::kOptionsStateRangeEdit &&
           platform::Read(static_cast<const std::uint8_t*>(event) + facts::kActionEventRepeat, repeat) && repeat == 0 &&
           platform::Read(static_cast<const std::uint8_t*>(event) + facts::kActionEventKey, key) &&
           platform::Read(g_acceptKey, accept) && accept != 0 && key == accept;
}

/// The page's key handler (vtable slot): a Range row's Accept queued first (the game's handler sets the value inline,
/// never through the commit the mod redirects; the row's refresh inside it then reads the new value), then the game's,
/// then the first page back if Back closed the mod's page.
std::uintptr_t KeyDown(void* page, void* a, void* b, void* event)
{
    if (RangeAccept(page, event))
    {
        coop_page::CommitEdit(page);
    }
    const std::uintptr_t reply = reinterpret_cast<KeyDownFn>(g_keyDown.Original())(page, a, b, event);
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
    g_acceptKey    = image.Rebase(f::kKeyAccept, 8).value_or(0);  // without it a slider's Accept commits nothing (logged
                                                                  // by menu/coop_slider)
    // Without them the page shows the first page's own guide (logged): the page still works.
    const auto guideSlot   = image.Rebase(f::kTopGuideSlot.slot, 8);
    const auto topGuide    = platform::FindFunction(image, f::kTopGuideSlot.target);
    const auto windowGuide = platform::FindFunction(image, f::kWindowGuideGetter);
    if (guideSlot && topGuide && windowGuide)
    {
        g.guideSlot   = *guideSlot;
        g.topGuide    = *topGuide;
        g.windowGuide = *windowGuide;
    }
    else
    {
        platform::LogWarning(L"Co-op page: the guide getters were not found; the page shows the main menu's guide");
    }
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
    SwapGuide(false);  // also off the game thread at an unload: one atomic pointer write
    g_keyDown.Restore();
    ReleaseTop();
}

void Remove() noexcept
{
    if (coop_page::IsOpen())
    {
        coop_page::SetOpen(false);
        platform::Log(L"Co-op page: the mod unloads with the page open; the page is left to the game");
    }
    g_atTitle   = false;
    g_titleHost = 0;
    ReleaseTop();  // a flag in the object array: no UI call
}

bool InstallTitle(const platform::GameImage& image) noexcept
{
    const auto openOptions = platform::FindFunction(image, facts::kStartMenuOpenOptions);
    const auto closed      = platform::FindFunction(image, facts::kStartMenuOptionsClosed);
    const auto setStick    = platform::FindFunction(image, facts::kSetStickDPadMode);
    if (!openOptions || !closed || !setStick)
    {
        return false;
    }
    g_title.openOptions = reinterpret_cast<TitleCallFn>(*openOptions);
    g_title.closed      = reinterpret_cast<ClosedFn>(*closed);
    g_title.setStick    = reinterpret_cast<SetStickFn>(*setStick);
    return true;
}

void OpenAtTitle(void* start) noexcept
try
{
    if (coop_page::IsOpen() || coop_page::Failed() || g_title.openOptions == nullptr)
    {
        return;
    }
    const std::uint64_t host = weak::WeakOf(start);
    bool                inRoot = false;
    void* const         page   = host != 0 ? coop_page::Prepare(start, std::uint8_t{0}, inRoot) : nullptr;
    if (page == nullptr)
    {
        if (host == 0)
        {
            platform::LogWarning(L"Co-op page: the title menu has no weak pointer; not opened");
        }
        return;
    }
    g_atTitle   = true;
    g_titleHost = host;
    // The game's open makes and adds its frame, then reads its page from one field (no parameter for it): adds it at its
    // order if it is not in the overlay (its construct runs then, over the game's data), binds its close function and
    // opens it. The field is the game's again before any other code can read it.
    void* const gamesPage = platform::ReadPointer(start, facts::kStartMenuPage);
    bool        opened    = platform::WriteGuarded(At(start, facts::kStartMenuPage), &page, sizeof(page));
    opened                = opened && platform::CallGuarded(g_title.openOptions, start);
    if (!platform::WriteGuarded(At(start, facts::kStartMenuPage), &gamesPage, sizeof(gamesPage)))
    {
        platform::LogWarning(L"Co-op page: the title menu's Options page could not be put back");
    }
    if (!opened)
    {
        LeftAtTitle(page, L"its opening faulted");
        return;
    }
    if (!coop_page::Fill(page))  // the page's construct has run: the mod's data over the game's
    {
        LeftAtTitle(page, L"writing it failed");
        return;
    }
    SwapHeadings(true);
    coop_page::SetOpen(true);
    ++g_opens;
    platform::Log(L"Co-op page: opened over the title menu ({} categories)", coop_page::Categories().size());
}
catch (...)
{
    if (g_atTitle)
    {
        LeftAtTitle(coop_page::Page(), L"an exception while opening");
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
    g_topWeak = topWeak;
    HoldTop(top);
    g_savedTitle = game_ui::ReadString(At(top, facts::kWindowTitle));
    game_ui::AssignString(At(top, facts::kWindowTitle), rules::kTitle);
    SwapGuide(true);
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
    if (!coop_page::IsOpen())
    {
        SwapGuide(false);
        ReleaseTop();  // thrown after the hold, before the page opened: nothing else would let it go
    }
}

void Tick() noexcept
{
    if (!coop_page::IsOpen())
    {
        return;
    }
    if (g_atTitle)
    {
        void* const titlePage = coop_page::Page();
        if (titlePage == nullptr)
        {
            LeftAtTitle(nullptr, L"the page is gone");
        }
        else if (weak::ObjectOf(g_titleHost) == nullptr)
        {
            LeftAtTitle(titlePage, L"the title menu is gone");
        }
        else if (PageClosing(titlePage))
        {
            ClosedAtTitle(L"seen by the tick");
        }
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
            ReleaseTop();
            g_mgr = nullptr;
            platform::Log(L"Co-op page: the page is gone with the main menu closing; the first page's title and guide "
                          L"put back");
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
