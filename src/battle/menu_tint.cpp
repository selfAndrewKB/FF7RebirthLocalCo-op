#include "battle/menu_tint.h"

#include "battle/command_menu.h"
#include "battle/command_menu_tick_shared.h"
#include "battle/menu_rules.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
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
constexpr std::uint8_t kMenuSoundBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x70, 0x8F, 0x75, 0x01};
using facts::kFilterOn;
using facts::kFilterOff;
constexpr platform::FunctionFact kMenuSound{L"EndSound_PostMenuEventIdx", 0x140A12C04, kMenuSoundBytes};
using facts::kRefInc;
using facts::kRefDec;
// The wait's muffle at its open: the sound event (event table entry 12) and the audio mode "WaitMode", both inlined in
// the wait machine (research/RE-menu-slow.md).
constexpr std::uint8_t kSoundEventBytes[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0xB8, 0x38, 0x04,
                                             0x00, 0x00, 0xE8, 0xD1, 0x8F, 0x75, 0x01, 0x48};
constexpr std::uint8_t kSoundModeBytes[]  = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                             0x90, 0x8D, 0x75, 0x01, 0x48, 0x2B, 0xE0, 0xC5};
constexpr platform::FunctionFact kSoundEvent{L"EndSound_PostEvent", 0x140A12BA0, kSoundEventBytes};
constexpr platform::FunctionFact kSoundMode{L"EndSound_SetAudioModeByName", 0x140A12DE4, kSoundModeBytes};
constexpr std::uintptr_t kFilterOnCall    = 0x14088B08B;  // wait mode 0 -> 1
constexpr std::uintptr_t kSoundOnCall     = 0x14088B097;  // 0 -> 1, the muffle's event
constexpr std::uintptr_t kSoundModeOnCall = 0x14088B0C7;  // 0 -> 1, the muffle's audio mode
constexpr std::uintptr_t kFilterOffCall   = 0x14088AF81;  // 5 -> 4
constexpr std::uintptr_t kSoundOffCall    = 0x14088AF93;  // 4 -> 0 (event 13, the audio mode cleared)
// EndSound_PostMenuEventIdx's two wait entries: 12 the muffle's event and audio mode together, 13 their end.
constexpr std::uint8_t kWaitSoundOn  = 12;
constexpr std::uint8_t kWaitSoundOff = 13;
using facts::kUIFilterManager;  // EndUISystem[41]
using facts::kFilterLatch;
using facts::kFilterCount;
constexpr std::uint8_t   kTintType      = 0;
using facts::kUICommandController;
using facts::kCmdWaitFilter;

using FilterOnFn       = void (*)(void* world, std::uint8_t type);
using FilterOffFn      = void (*)(std::uint8_t type);
using MenuSoundFn      = void (*)(std::uint8_t index);
using FilterCountFn    = void (*)(void* manager, std::uint8_t type);
using SoundEventFn     = void (*)(void* event);
using SoundModeFn      = void (*)(std::uint64_t mode, float fade);  // the mode's FName, by value

// Set once at install.
platform::CallRedirect g_filterOnCall, g_filterOffCall, g_soundOffCall, g_soundOnCall, g_soundModeOnCall;
FilterCountFn          g_refInc = nullptr, g_refDec = nullptr;

/// The calls' targets as Find found them, for the redirects (placed at once, or on 1.005 from the game thread).
struct Targets
{
    std::uintptr_t filterOn = 0, filterOff = 0, menuSound = 0, soundEvent = 0, soundMode = 0;
};
Targets g_targets;

// Game thread.
// Each seat's menu holds one count of the tint (the game's own owner-byte pattern); by seat, index 0 unused (player
// 1's menu keeps the game's latch).
std::array<bool, players::kMaxPlayers> g_holdsTint{};
int                                    g_tintLines = 0;
bool                                   g_followed  = true;  // the slow-down switch as the waiting menus last followed it
std::uint32_t g_greysSkipped = 0, g_mufflesSkipped = 0, g_switchesFollowed = 0;

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
/// its own. Skipped with the slow-down switched off: no latch and no count are taken, so the menu's close and the
/// game's restores after another menu find nothing to bring back.
void TintOn(void* world, std::uint8_t type) noexcept
{
    const int seat = command_menu_tick::PassSeat();
    if (!command_menu_tick::MenuSlow())
    {
        ++g_greysSkipped;
        TintLine(command_menu_tick::Pass() == 2 ? seat : 0, L"skipped (the slow-down is switched off)");
        return;
    }
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

/// Replaces the wait mode's sound event at its open (the muffle's first half): the game's call while the slow-down is
/// on, else skipped.
void WaitSoundOn(void* event) noexcept
{
    if (command_menu_tick::MenuSlow())
    {
        reinterpret_cast<SoundEventFn>(g_soundOnCall.Original())(event);
        return;
    }
    ++g_mufflesSkipped;
    platform::LogDebug(L"menu sound: a battle menu opened; the muffle is skipped (the slow-down is switched off)");
}

/// Replaces the wait mode's audio mode at its open (the muffle's second half): the same rule.
void WaitSoundModeOn(std::uint64_t mode, float fade) noexcept
{
    if (command_menu_tick::MenuSlow())
    {
        reinterpret_cast<SoundModeFn>(g_soundModeOnCall.Original())(mode, fade);
    }
}

/// Replaces the wait mode's sound off (event 13 and the audio mode cleared): skipped with the slow-down switched off
/// (nothing was raised; a switch while the menu waited settled it), and while another menu is still in wait mode.
/// The menus tick in order (player 1's first, then each seat's), so if several close in one frame the last one plays
/// it.
void WaitSoundOff(std::uint8_t index) noexcept
{
    if (!command_menu_tick::MenuSlow())
    {
        return;
    }
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

void FollowSwitch() noexcept
{
    const bool slows = command_menu_tick::MenuSlow();
    if (slows == g_followed || !g_filterOnCall.IsInstalled())
    {
        return;
    }
    g_followed = slows;
    ++g_switchesFollowed;
    const auto waits = Waits();
    const auto fm    = FilterManager();
    // Player 1's menu holds the game's latch.
    const bool latched = fm != 0 && ReadAt<std::uint8_t>(fm + kFilterLatch + kTintType) != 0;
    switch (menu_rules::GreyAtSwitch(slows, waits[0], latched))
    {
        case menu_rules::Flip::Lift:
            platform::CallGuarded(reinterpret_cast<FilterOffFn>(g_targets.filterOff), kTintType);
            TintLine(0, L"taken down for player 1's menu (the slow-down switched off)");
            break;
        case menu_rules::Flip::Raise:
            platform::CallGuarded(reinterpret_cast<FilterOnFn>(g_targets.filterOn), static_cast<void*>(nullptr), kTintType);
            TintLine(0, L"raised for player 1's waiting menu (the slow-down switched on)");
            break;
        case menu_rules::Flip::Nothing:
            break;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        auto& holds = g_holdsTint[static_cast<std::size_t>(seat)];
        switch (menu_rules::GreyAtSwitch(slows, waits[static_cast<std::size_t>(seat)], holds))
        {
            case menu_rules::Flip::Lift:
                Release(seat, L"taken down (the slow-down switched off)");
                break;
            case menu_rules::Flip::Raise:
                if (fm != 0 && platform::CallGuarded(g_refInc, reinterpret_cast<void*>(fm), kTintType))
                {
                    holds = true;
                }
                TintLine(seat, L"raised for the waiting menu (the slow-down switched on)");
                break;
            case menu_rules::Flip::Nothing:
                break;
        }
    }
    const menu_rules::Flip sound = menu_rules::SoundAtSwitch(slows, waits.data(), 1 + players::kSeatsBuilt);
    if (sound != menu_rules::Flip::Nothing)
    {
        const bool raise = sound == menu_rules::Flip::Raise;
        if (platform::CallGuarded(reinterpret_cast<MenuSoundFn>(g_targets.menuSound), raise ? kWaitSoundOn : kWaitSoundOff))
        {
            platform::LogDebug(L"menu sound: the muffle {} for a waiting menu (the slow-down switched {})",
                               raise ? L"raised" : L"lifted", slows ? L"on" : L"off");
        }
        else
        {
            platform::LogWarning(L"menu sound: the game's sound call raised; the muffle may not follow the switch until the "
                                 L"menu closes");
        }
    }
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"menu-tint",
                       .shape = control::Shape::Text,
                       .help  = L"an open battle menu's grey and muffle under the slow-down switch: the switch, the greys "
                                L"and muffles skipped, the switches followed",
                       .get   = [] {
                           try
                           {
                               return control::Value{std::format(
                                   L"the slow-down {}; calls {}; greys skipped {}, muffles skipped {}, switches followed {}",
                                   command_menu_tick::MenuSlow() ? L"on" : L"off",
                                   g_soundModeOnCall.IsInstalled() ? L"placed" : L"not placed", g_greysSkipped,
                                   g_mufflesSkipped, g_switchesFollowed)};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}

#endif

bool Find(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto filterOn = find(kFilterOn), filterOff = find(kFilterOff), menuSound = find(kMenuSound),
               refInc = find(kRefInc), refDec = find(kRefDec), soundEvent = find(kSoundEvent), soundMode = find(kSoundMode);
    if (!filterOn || !filterOff || !menuSound || !refInc || !refDec || !soundEvent || !soundMode)
    {
        return false;
    }
    g_refInc  = reinterpret_cast<FilterCountFn>(*refInc);
    g_refDec  = reinterpret_cast<FilterCountFn>(*refDec);
    g_targets = {*filterOn, *filterOff, *menuSound, *soundEvent, *soundMode};
    return true;
}

bool Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept
{
    const auto redirect = [&](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && placement.Place(call, *live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    // The muffle's two on calls cross a 16-byte block on 1.004 (only the opcode in the first). They are
    // placed from their own thread's side: at install no world exists, so no thread can be in the wait machine;
    // otherwise the placement runs on the game thread between its ticks. After the grey's: the first block holds the
    // grey's call too, and Remove puts the muffle's back first.
    const auto ownThread = [&](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && call.InstallFromOwnThread(*live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    g_followed = command_menu_tick::MenuSlow();
    return redirect(g_filterOnCall, kFilterOnCall, g_targets.filterOn, &TintOn) &&
           redirect(g_filterOffCall, kFilterOffCall, g_targets.filterOff, &TintOff) &&
           redirect(g_soundOffCall, kSoundOffCall, g_targets.menuSound, &WaitSoundOff) &&
           ownThread(g_soundOnCall, kSoundOnCall, g_targets.soundEvent, &WaitSoundOn) &&
           ownThread(g_soundModeOnCall, kSoundModeOnCall, g_targets.soundMode, &WaitSoundModeOn);
}

void Remove() noexcept
{
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        Release(seat, L"released: the stage is removed");
    }
    // The muffle's first: their first block holds the grey's redirected call in its snapshot.
    for (auto* call : {&g_soundModeOnCall, &g_soundOnCall, &g_filterOnCall, &g_filterOffCall, &g_soundOffCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::menu_tint
