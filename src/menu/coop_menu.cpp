#include "menu/coop_menu.h"

#include "facts/menu.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/coop_page_open.h"
#include "menu/game_ui.h"
#include "menu/title_row.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::menu::coop_menu {

namespace {

namespace rules = coop_menu_rules;

using game_ui::At;
using OpenWindowFn = coop_page_open::OpenWindowFn;

/// Where the row's opener and table words go, found at install.
struct Game
{
    std::uintptr_t openWindowAt = 0;
    std::uintptr_t textSlot = 0, textWas = 0, helpSlot = 0, helpWas = 0, orderWord = 0;
    std::uintptr_t demoName = 0;                  // 1.004: the builder's name global
    std::uintptr_t demoCall = 0, demoTest = 0;    // 1.005: the builder's call of the demo-mode test, and its target
};

// Set at install and on the first tick, then only read.
Game                    g;
platform::EntryRedirect g_openWindow;
platform::VTableEntry   g_text, g_help, g_order, g_demoName;
platform::CallRedirect  g_demoCall;
bool                    g_installed = false;
bool                    g_pending   = false;  // the first tick places everything
bool                    g_placed    = false;  // the row shows and opens the page
bool                    g_titleFound = false;  // the title menu can have its row

/// The game's window opener: the Co-op row's kind opens the page; every other kind is the game's.
void OpenWindow(void* mgr, std::uint8_t kind)
{
    const auto original = reinterpret_cast<OpenWindowFn>(g_openWindow.Original());
    if (kind == facts::kTopRowKind && g_placed && mgr != nullptr)
    {
        coop_page_open::Open(mgr, original);
        return;
    }
    original(mgr, kind);
}

/// 1.005: the row builder's demo-mode test, answered yes (its one call there).
bool BuilderAppendsRow()
{
    return true;
}

/// The builder's gate for kind 13, per build.
bool PlaceGate() noexcept
{
    if (g.demoCall != 0)
    {
        return g_demoCall.InstallFromOwnThread(g.demoCall, g.demoTest, reinterpret_cast<std::uintptr_t>(&BuilderAppendsRow));
    }
    std::uintptr_t was = 0;
    return platform::Read(g.demoName, was) && g_demoName.Replace(g.demoName, was, 0);
}

/// The redirects, the key handler's slot, then the table words (the row last, so it never shows unopenable).
bool Place(const platform::GameImage& image) noexcept
{
    const auto first = image.EntryInstruction(facts::kOpenWindowByKind.address,
                                              facts::kOpenWindowByKind.entryBytes.first(facts::kOpenWindowByKindFirst));
    const bool redirected = g_openWindow.Install(g.openWindowAt, first, reinterpret_cast<std::uintptr_t>(&OpenWindow)) &&
                            coop_page::Place(image) && coop_page_open::Place(image);
    if (!redirected)
    {
        return false;
    }
    // The title's row is its own: without it the title stays as it was (logged by title_row).
    if (g_titleFound)
    {
        title_row::Place(image);
    }
    return g_text.Replace(g.textSlot, g.textWas, reinterpret_cast<std::uintptr_t>(rules::kRowLabel)) &&
           g_help.Replace(g.helpSlot, g.helpWas, reinterpret_cast<std::uintptr_t>(rules::kRowHelp)) &&
           g_order.Replace(g.orderWord, facts::kTopRowOrderWas, facts::kTopRowOrderCoop) && PlaceGate();
}

void Unplace() noexcept
{
    // The gate and the row's tables first (the row no longer built), then the page's parts and the opener. The 1.005 gate
    // crosses a block and goes back only from the game thread; from another (an unload) everything stays, the DLL being
    // pinned: a row still built after a half put-back would read "Buy Now" and run the trial's action.
    if (g_demoCall.IsInstalled())
    {
        if (!players::generation::OnGameThread())
        {
            platform::Log(L"Co-op menu: the mod unloads off the game thread; the row and its opener stay (they pass to "
                          L"the mod's pinned code)");
            return;
        }
        g_demoCall.Restore();
    }
    title_row::Unplace();
    g_demoName.Restore();
    g_order.Restore();
    g_help.Restore();
    g_text.Restore();
    coop_page_open::Unplace();
    coop_page::Unplace();
    g_openWindow.Restore();
}

std::wstring Reading()
{
    std::wstring text = std::format(L"row {}; page {}{}; opened {}, values set {}", g_placed ? L"placed" : L"not placed",
                                    coop_page::IsOpen() ? L"open" : L"closed",
                                    coop_page::Failed() ? L" (failed: opens nothing)" : L"", coop_page_open::Opens(),
                                    coop_page::Commits());
    text += title_row::Reading();
    void* const         mgr = game_ui::Manager(facts::kUiMainMenuSlot);
    void* const         top = mgr != nullptr ? game_ui::WeakAt(mgr, facts::kMainMenuMgrCurrent) : nullptr;
    platform::GameArray rows;
    if (game_ui::IsTopWindow(top) && platform::Read(At(top, facts::kTopRows), rows) && rows.data != nullptr && rows.num > 0 &&
        rows.num <= 16)
    {
        std::uint8_t kinds[16]{};
        if (platform::CopyGuarded(kinds, rows.data, static_cast<std::size_t>(rows.num)))
        {
            text += L"; main menu rows (kinds):";
            for (std::int32_t i = 0; i < rows.num; ++i)
            {
                text += std::format(L" {}", kinds[i]);
            }
        }
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f           = facts;
    const auto openWindow = platform::FindFunction(image, f::kOpenWindowByKind);
    const auto textSlot   = image.Rebase(f::kTopRowTextSlot, 8);
    const auto textWas    = image.Rebase(f::kTopRowTextWas);
    const auto helpSlot   = image.Rebase(f::kTopRowHelpSlot, 8);
    const auto helpWas    = image.Rebase(f::kTopRowHelpWas);
    const auto orderWord  = image.Rebase(f::kTopRowOrderWord, 8);
    // The builder's gate: 1.004's name global through the table; 1.005's call at this build's own address, its bytes
    // checked (no research address: 1.004 has no such call).
    bool gate = false;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        const std::uintptr_t call = image.Base() + (f::kTopRowDemoCall - 0x140000000);
        const std::uintptr_t test = image.Base() + (f::kTopRowDemoTest - 0x140000000);
        std::uint8_t         bytes[sizeof f::kTopRowDemoCallBytes]{};
        gate = image.Contains(call) && image.Contains(test) && platform::CopyGuarded(bytes, reinterpret_cast<const void*>(call), sizeof bytes) &&
               std::memcmp(bytes, f::kTopRowDemoCallBytes, sizeof bytes) == 0;
        g.demoCall = gate ? call : 0;
        g.demoTest = gate ? test : 0;
    }
    else
    {
        const auto name = image.Rebase(f::kTopRowDemoName, 8);
        gate            = name.has_value();
        g.demoName      = name.value_or(0);
    }
    // The shared pieces first: the page writes its strings with their copy.
    if (!openWindow || !textSlot || !textWas || !helpSlot || !helpWas || !orderWord || !gate || !game_ui::Install(image) ||
        !coop_page::Install(image) || !coop_page_open::Install(image))
    {
        platform::Log(L"Co-op menu: an engine check failed; no Co-op row in the main menu (the options stay on the console "
                      L"and the settings file)");
        return false;
    }
    g.openWindowAt = *openWindow;
    g.textSlot     = *textSlot;
    g.textWas      = *textWas;
    g.helpSlot     = *helpSlot;
    g.helpWas      = *helpWas;
    g.orderWord    = *orderWord;
    g_titleFound   = title_row::Install(image);
    g_installed    = true;
    g_pending      = true;  // placed by the first tick, on the game thread the redirected functions run on
    return true;
}
catch (...)
{
    return false;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // The unload may run off the game thread: no UI calls here, only the mod's own state and its changes put back.
    coop_page_open::Remove();
    g_placed = false;
    Unplace();
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    if (g_pending)
    {
        g_pending = false;
        if (const auto image = platform::GameImage::Locate(); image && Place(*image))
        {
            g_placed = true;
            platform::Log(L"Co-op menu: a Co-op row in the main menu before System (the trial's Buy Now row, which the full "
                          L"game never shows), opening the game's Options screen with the mod's options in {} "
                          L"categories",
                          coop_page::Categories().size());
        }
        else
        {
            Unplace();
            platform::Log(L"Co-op menu: the row's opener or tables could not be placed; no Co-op row (the options stay on "
                          L"the console and the settings file)");
        }
    }
    coop_page::Tick();
    coop_page_open::Tick();
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"menu.coop",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the Co-op row of the main menu and its page: placed, open, opened and set "
                                L"counts, and the main menu's rows by kind while its first page is current",
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

}  // namespace lss::menu::coop_menu
