#include "battle/menu_tint.h"

#include "battle/command_menu.h"
#include "battle/command_menu_tick_shared.h"
#include "battle/menu_rules.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/object_fields.h"
#include "players/seats.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::menu_tint {

namespace {

// The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by every controller's menu
// (RE-p2-command-menu-tint.md). EnableType/DisableType are a latch over a real reference count, so two
// owners cancel each other; the game's other menus share a filter through the count itself
// (EndUIFilter_RefInc/RefDec on EndUISystem[41], keeping their own "holds it" byte). Each seat's menu does
// the same; player 1's keeps the latch. The wait sound's off is one mode slot: it is skipped while the
// other menu is still in wait mode.
constexpr std::uint8_t kFilterOnBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE8, 0xD3, 0xEB, 0x00};
constexpr std::uint8_t kFilterOffBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xF0, 0xDA, 0xEB, 0x00};
constexpr std::uint8_t kMenuSoundBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xE8, 0x50, 0x51, 0x01};
constexpr std::uint8_t kRefIncBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                            0x00, 0x00, 0x00, 0xE8, 0xAC, 0xD3, 0xEB, 0x00};
constexpr std::uint8_t kRefDecBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                            0x00, 0x00, 0x00, 0xE8, 0x74, 0xDA, 0xEB, 0x00};
constexpr platform::FunctionFact kFilterOn{L"EndUIFilter_EnableType", 0x1411F1B3C, kFilterOnBytes};
constexpr platform::FunctionFact kFilterOff{L"EndUIFilter_DisableType", 0x1411F1434, kFilterOffBytes};
constexpr platform::FunctionFact kMenuSound{L"EndSound_PostMenuEventIdx", 0x140B99E3C, kMenuSoundBytes};
constexpr platform::FunctionFact kRefInc{L"EndUIFilter_RefInc", 0x1411F1B74, kRefIncBytes};
constexpr platform::FunctionFact kRefDec{L"EndUIFilter_RefDec", 0x1411F14AC, kRefDecBytes};
constexpr std::uintptr_t kFilterOnCall  = 0x140963943;  // wait mode 0 -> 1
constexpr std::uintptr_t kFilterOffCall = 0x140963839;  // 5 -> 4
constexpr std::uintptr_t kSoundOffCall  = 0x14096384B;  // 4 -> 0 (event 13, the audio mode cleared)
using facts::kUIFilterManager;  // EndUISystem[41]
constexpr std::uint32_t  kFilterLatch   = 0x1C;         // u8 per type
constexpr std::uint32_t  kFilterCount   = 0x2C;         // i32 per type
constexpr std::uint8_t   kTintType      = 0;
using facts::kUICommandController;
using facts::kCmdWaitFilter;

using FilterOnFn       = void (*)(void* world, std::uint8_t type);
using FilterOffFn      = void (*)(std::uint8_t type);
using MenuSoundFn      = void (*)(std::uint8_t index);
using FilterCountFn    = void (*)(void* manager, std::uint8_t type);

// Set once at install.
platform::CallRedirect g_filterOnCall, g_filterOffCall, g_soundOffCall;
FilterCountFn          g_refInc = nullptr, g_refDec = nullptr;

/// The calls' targets as Find found them, for the redirects (placed at once, or on 1.005 from the game thread).
struct Targets
{
    std::uintptr_t filterOn = 0, filterOff = 0, menuSound = 0;
};
Targets g_targets;

// Game thread.
// Each seat's menu holds one count of the tint (the game's own owner-byte pattern); by seat, index 0 unused (player
// 1's menu keeps the game's latch).
std::array<bool, players::kMaxPlayers> g_holdsTint{};
int                                    g_tintLines = 0;

/// The tint's filter manager and, for the log, its latch and count for the tint type.
std::uintptr_t FilterManager() noexcept
{
    return ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUIFilterManager);
}

/// Every menu's wait state, by seat (player 1's at 0; 0 for a seat without a controller).
std::array<std::int32_t, players::kMaxPlayers> Waits() noexcept
{
    const auto wait = [](std::uintptr_t ctrl) { return ctrl != 0 ? ReadAt<std::int32_t>(ctrl + kCmdWaitFilter) : 0; };
    std::array<std::int32_t, players::kMaxPlayers> waits{};
    waits[0] = wait(ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUICommandController));
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        waits[static_cast<std::size_t>(seat)] =
            wait(reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat)));
    }
    return waits;
}

/// `seat`: the menu the line is about (0 player 1's).
void TintLine(int seat, const wchar_t* what) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || ++g_tintLines > 60)
    {
        return;
    }
    try
    {
        const auto   fm  = FilterManager();
        const auto   one = ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUICommandController);
        std::wstring waits = std::format(L"P1 {}", one != 0 ? ReadAt<std::int32_t>(one + kCmdWaitFilter) : -1);
        std::wstring holds;
        for (int other = 1; other <= players::kSeatsBuilt; ++other)
        {
            const auto own = reinterpret_cast<std::uintptr_t>(command_menu::Controller(other));
            waits += std::format(L" P{} {}", other + 1, own != 0 ? ReadAt<std::int32_t>(own + kCmdWaitFilter) : -1);
            holds += std::format(L"{}P{} holds {}", other == 1 ? L"" : L", ", other + 1,
                                 g_holdsTint[static_cast<std::size_t>(other)] ? 1 : 0);
        }
        platform::LogDebug(L"command menu tick: tint {}{}{} (pass {}; wait {}; latch {} count {}; {})", what,
                           seat >= 1 ? L" for " : L"", seat >= 1 ? players::SeatName(seat) : L"",
                           command_menu_tick::Pass(), waits, ReadAt<std::uint8_t>(fm + kFilterLatch + kTintType),
                           ReadAt<std::int32_t>(fm + kFilterCount + 4 * kTintType), holds);
    }
    catch (...)
    {
    }
}

}  // namespace

/// `seat`'s count of the tint given back (its menu closed, or it stops holding for another reason).
void Release(int seat, const wchar_t* why) noexcept
{
    if (!players::SeatIndex(seat) || !g_holdsTint[static_cast<std::size_t>(seat)])
    {
        return;
    }
    const auto fm = FilterManager();
    if (fm != 0)
    {
        platform::CallGuarded(g_refDec, reinterpret_cast<void*>(fm), kTintType);
    }
    g_holdsTint[static_cast<std::size_t>(seat)] = false;
    TintLine(seat, why);
}

bool Holds(int seat) noexcept
{
    return players::SeatIndex(seat) && g_holdsTint[static_cast<std::size_t>(seat)];
}

namespace {

/// Replaces the wait mode's filter on: player 1's menu takes the latch as the game does; a seat's takes a count of
/// its own.
void TintOn(void* world, std::uint8_t type) noexcept
{
    const int seat = command_menu_tick::PassSeat();
    if (command_menu_tick::Pass() != 2 || !players::SeatIndex(seat))
    {
        reinterpret_cast<FilterOnFn>(g_filterOnCall.Original())(world, type);
        return;
    }
    const auto fm = FilterManager();
    auto&      holds = g_holdsTint[static_cast<std::size_t>(seat)];
    if (!holds && fm != 0 && platform::CallGuarded(g_refInc, reinterpret_cast<void*>(fm), type))
    {
        holds = true;
    }
    TintLine(seat, L"on (a count)");
}

/// Replaces the wait mode's filter off: a seat's menu gives back its count; player 1's drops the latch.
void TintOff(std::uint8_t type) noexcept
{
    const int seat = command_menu_tick::PassSeat();
    if (command_menu_tick::Pass() != 2 || !players::SeatIndex(seat))
    {
        reinterpret_cast<FilterOffFn>(g_filterOffCall.Original())(type);
        TintLine(0, L"off for player 1's menu (the latch)");
        return;
    }
    Release(seat, L"off (its count)");
}

/// Replaces the wait mode's sound off (event 13 and the audio mode cleared): skipped while another menu is still in
/// wait mode. The menus tick in order (player 1's first, then each seat's), so if several close in one frame the last
/// one plays it.
void WaitSoundOff(std::uint8_t index) noexcept
{
    const int  pass  = command_menu_tick::Pass();
    const int  me    = pass == 1 ? 0 : pass == 2 ? command_menu_tick::PassSeat() : -1;
    const auto waits = Waits();
    if (menu_rules::SkipWaitSoundOffAmong(me, waits.data(), 1 + players::kSeatsBuilt))
    {
        TintLine(std::max(me, 0), L"wait sound off skipped (another menu is still in wait mode)");
        return;
    }
    reinterpret_cast<MenuSoundFn>(g_soundOffCall.Original())(index);
}

}  // namespace

bool Find(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto filterOn = find(kFilterOn), filterOff = find(kFilterOff), menuSound = find(kMenuSound),
               refInc = find(kRefInc), refDec = find(kRefDec);
    if (!filterOn || !filterOff || !menuSound || !refInc || !refDec)
    {
        return false;
    }
    g_refInc  = reinterpret_cast<FilterCountFn>(*refInc);
    g_refDec  = reinterpret_cast<FilterCountFn>(*refDec);
    g_targets = {*filterOn, *filterOff, *menuSound};
    return true;
}

bool Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept
{
    const auto redirect = [&](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && placement.Place(call, *live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    return redirect(g_filterOnCall, kFilterOnCall, g_targets.filterOn, &TintOn) &&
           redirect(g_filterOffCall, kFilterOffCall, g_targets.filterOff, &TintOff) &&
           redirect(g_soundOffCall, kSoundOffCall, g_targets.menuSound, &WaitSoundOff);
}

void Remove() noexcept
{
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        Release(seat, L"released: the stage is removed");
    }
    for (auto* call : {&g_filterOnCall, &g_filterOffCall, &g_soundOffCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::menu_tint
