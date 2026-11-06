#include "hud/minimap/seat_area_icons.h"

#include "facts/engine.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::seat_area_icons {

namespace {

// Facts read from the database (RE-p2-hud-icon-manager.md §1, §4 and §8; this build). Each
// function's prologue up to its stack probe call is checked before it is called or redirected.
constexpr std::uintptr_t kInstanceOne = 0x149349880;  // the game's instance (EndUISystem+0x1A0), a pointer
constexpr std::size_t    kInstanceSize = 0x160;

constexpr std::uint8_t   kCtorPrologue[]   = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// AreaMapIconMgr__Ctor(memory), returns it
constexpr platform::FunctionFact kCtor{L"AreaMapIconMgr__Ctor", 0x143A0D944, kCtorPrologue};
constexpr std::uint8_t   kFrameUpdatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41,
                                                   0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x50, 0x07, 0x00, 0x00};
// AreaMapIcon_FrameUpdate(instance, xmm1 = DeltaTime)
constexpr platform::FunctionFact kFrameUpdate{L"AreaMapIcon_FrameUpdate", 0x1409F72B8, kFrameUpdatePrologue};
constexpr std::uint8_t   kClearAllPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
                                                0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0xB8};
// AreaMapIconMgr__ClearAllAndDisable(instance)
constexpr platform::FunctionFact kClearAll{L"AreaMapIconMgr__ClearAllAndDisable", 0x140F5D854, kClearAllPrologue};

// The frame function's only call for the first player controller (§8.2): E8 rel32 at 0x1409F7439
// to UWorld__GetFirstPlayerController, rcx = the world, inside the block at 0x1409F7430.
constexpr std::uintptr_t kFirstControllerCall = 0x1409F7439;

// The frame function's call for the range origin, the leader character by the leader's player type
// (RE-p2-hud-targets.md §1.1, R1): E8 rel32 at 0x1409F73E4 to Party_GetLeaderCharacterByLeaderType,
// no arguments, inside the block at 0x1409F73E0.
constexpr std::uintptr_t kLeaderCall = 0x1409F73E4;
constexpr std::uint8_t   kLeaderPrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x46, 0xD1, 0x42, 0x01};
constexpr platform::FunctionFact kLeader{L"Party_GetLeaderCharacterByLeaderType", 0x140C81DE0, kLeaderPrologue};

// The three game events that write player 1's instance, and their seven call sites
// (RE-p2-hud-view-levers.md §6 and S-HUD31). None takes the instance: each reads EndUISystem+0x1A0.
// Each site keeps calling the event, then the event's instance effect is applied to each seat's
// instance directly. The events themselves are not run again: Enable and HideAndClear also play
// the in/out animation of one shared widget (EndUISystem+0x60), which a second run would restart.
constexpr std::uint8_t   kEnablePrologue[]    = {0xB8, 0x28, 0x00, 0x00, 0x00};
// void(): +0xD0 = 1, and the shared widget's "in"
constexpr platform::FunctionFact kEnable{L"AreaMapIconMgr Enable", 0x141816F1C, kEnablePrologue};
constexpr std::uint8_t   kHideAndClearPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// void(): ClearAllAndDisable, and the widget's "out"
constexpr platform::FunctionFact kHideAndClear{L"AreaMapIconMgr HideAndClear", 0x141816EE0, kHideAndClearPrologue};
constexpr std::uint8_t   kSetFlagD1Prologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// void(char): +0xD1 = cl
constexpr platform::FunctionFact kSetFlagD1{L"AreaMapIconMgr SetFlagD1", 0x1439D1CEC, kSetFlagD1Prologue};

enum class Event : std::uint8_t
{
    Enable,
    HideAndClear,
    SetFlagD1,
};

struct EventSite
{
    std::uintptr_t site;
    Event          event;
    bool           tailJump;  // E9: EndUI__AreaMapIcon_EnableOrHideByCount's tail calls
};
constexpr EventSite kEventSites[] = {
    {0x1410340A7, Event::HideAndClear, false},  // EventCmd_Dispatch_BattleModeCase87
    {0x1410340B1, Event::Enable, false},        // the same event command
    {0x143544CB0, Event::Enable, true},         // EndUI__AreaMapIcon_EnableOrHideByCount, count > 0
    {0x143544CB9, Event::HideAndClear, true},   // the same, count <= 0
    {0x14356050B, Event::SetFlagD1, false},     // a state's enter (cl = 0)
    {0x143561FD5, Event::SetFlagD1, false},     // that state's exit (cl = 1)
    {0x1435680F4, Event::SetFlagD1, false},     // a stop/cleanup (cl = 1)
};

// SWidget::SetVisibility(widget, TAttribute<EVisibility>*), canvas vtable slot 59 (§8.3).
constexpr std::uint8_t   kSetVisibilityPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetVisibility{L"SWidget::SetVisibility", 0x140C27B38, kSetVisibilityPrologue};
constexpr std::size_t    kSetVisibilitySlot       = 59;

// The instance (§1, §2): the canvases as TWeakPtr {object, reference controller} (#1 icons, #2 quest
// and party markers), the icon and marker maps, and the fields game events write.
constexpr std::array<std::uint32_t, 2> kCanvases{0x10, 0x70};
constexpr std::array<std::uint32_t, 2> kMaps{0x20, 0x80};
constexpr std::uint32_t kEnabled   = 0xD0;  // u8
constexpr std::uint32_t kFlagD1    = 0xD1;  // u8
constexpr std::uint32_t kSharedCount = 0x8;  // the weak pointer's reference controller: its shared count (int32)

// The canvas's Visibility attribute (§8.3): +0 value, +1 set, +8 getter delegate, +0x10 its size.
constexpr std::uint32_t kVisibility         = 0x1B0;
constexpr std::uint32_t kVisibilityDelegate = 0x1B8;
constexpr std::uint8_t  kHidden               = 0x02;
constexpr std::uint8_t  kSelfHitTestInvisible = 0x11;  // what the canvases are constructed with


enum Part : unsigned
{
    kPartTick       = 1,
    kPartController = 2,
    kPartCanvases   = 4,
    kPartLeader     = 8,  // a seat's icons ranged from the seat's member
};

struct VisibilityAttribute  // TAttribute<EVisibility> as the game passes it: unbound
{
    std::uint8_t  value = 0;
    std::uint8_t  isSet = 1;
    std::uint8_t  pad[6]{};
    std::uint64_t delegate = 0;
    std::int32_t  delegateSize = 0;
    std::int32_t  pad2 = 0;
};
static_assert(sizeof(VisibilityAttribute) == 24);

struct ShownCanvas  // a canvas of player 1's hidden for a seat's pass, and its own visibility
{
    std::uintptr_t canvas = 0;
    std::uint8_t   visibility = 0;
};

using MallocFn        = void* (*)(std::size_t size);
using CtorFn          = void* (*)(void* memory);
using FrameUpdateFn   = void (*)(void* instance, float deltaTime);
using ClearAllFn      = void (*)(void* instance);
using FirstControllerFn = void* (*)(void* world);
using LeaderFn          = void* (*)();
using SetVisibilityFn = void (*)(void* widget, VisibilityAttribute* attribute);
using EventFn         = void (*)();
using SetFlagFn       = void (*)(char flag);

// Set once at install.
platform::CallRedirect g_firstControllerCall;
platform::CallRedirect g_leaderCall;
bool                   g_installed      = false;
std::uintptr_t         g_instanceOne    = 0;  // the address holding the game's instance
MallocFn               g_malloc         = nullptr;
CtorFn                 g_ctor           = nullptr;
FrameUpdateFn          g_frameUpdate    = nullptr;
ClearAllFn             g_clearAll       = nullptr;
SetVisibilityFn        g_setVisibility  = nullptr;
EventFn                g_enable         = nullptr;
EventFn                g_hideAndClear   = nullptr;
SetFlagFn              g_setFlagD1      = nullptr;
std::array<platform::CallRedirect, std::size(kEventSites)> g_eventCalls;
bool                   g_eventsFollowed = false;

constexpr int kPlayers = players::kMaxPlayers;

// Game thread. By seat ([0] unused): each seat's own instance, made with the game's constructor and kept.
std::uintptr_t               g_instance[kPlayers]     = {};
bool                         g_createFailed[kPlayers] = {};
bool                         g_tickFailed[kPlayers]   = {};
bool                         g_seeded[kPlayers]       = {};  // the seat's instance has player 1's event state
std::array<std::uint64_t, 7> g_logged[kPlayers]       = {};
void*                        g_seatController = nullptr;  // answered while a seat's tick runs
void*                        g_seatMember     = nullptr;  // answered as the range origin while it runs
unsigned                     g_mask   = kPartTick | kPartController | kPartCanvases | kPartLeader;
std::array<ShownCanvas, 2>   g_hiddenOne{};
std::uint32_t                g_answered = 0;  // first-controller calls answered with a seat's
std::uint32_t                g_leaderAnswers = 0;  // range-origin calls answered with a seat's member
bool                         g_boundLogged = false;
bool                         g_offThreadLogged = false;
std::uint32_t                g_replays     = 0;

bool Acts(Part part) noexcept
{
    return (g_mask & part) != 0;
}

using platform::Read;  // the guarded reads: platform/guarded.h

/// Replaces the frame function's call for the first player controller.
void* FirstController(void* world) noexcept
{
    if (g_seatController != nullptr)
    {
        ++g_answered;
        return g_seatController;
    }
    return reinterpret_cast<FirstControllerFn>(g_firstControllerCall.Original())(world);
}

/// Replaces the frame function's call for the range origin: the seat's member in the seat's tick.
void* Leader() noexcept
{
    if (g_seatMember != nullptr)
    {
        ++g_leaderAnswers;
        return g_seatMember;
    }
    return reinterpret_cast<LeaderFn>(g_leaderCall.Original())();
}

std::uintptr_t CreateGuarded() noexcept
{
    __try
    {
        void* memory = g_malloc(kInstanceSize);
        return memory == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(g_ctor(memory));
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return 0;
    }
}

bool FrameUpdateGuarded(std::uintptr_t instance, float deltaTime) noexcept
{
    __try
    {
        g_frameUpdate(reinterpret_cast<void*>(instance), deltaTime);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool ClearAllGuarded(std::uintptr_t instance) noexcept
{
    __try
    {
        g_clearAll(reinterpret_cast<void*>(instance));
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool SetVisibilityGuarded(std::uintptr_t canvas, std::uint8_t value) noexcept
{
    __try
    {
        VisibilityAttribute attribute;
        attribute.value = value;
        g_setVisibility(reinterpret_cast<void*>(canvas), &attribute);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

std::uintptr_t InstanceOne() noexcept
{
    std::uintptr_t instance = 0;
    Read(g_instanceOne, instance);
    return instance;
}

/// The canvas a weak pointer at `instance + offset` holds, or 0 if it does not pin.
std::uintptr_t Canvas(std::uintptr_t instance, std::uint32_t offset) noexcept
{
    std::uintptr_t object     = 0;
    std::uintptr_t reference  = 0;  // the weak pointer's reference controller
    std::int32_t   shared     = 0;
    if (instance == 0 || !Read(instance + offset, object) || !Read(instance + offset + 8, reference) || object == 0 ||
        reference == 0 || !Read(reference + kSharedCount, shared) || shared <= 0)
    {
        return 0;
    }
    return object;
}

/// Whether the canvas's visibility can be set as the game sets it: unbound, and its vtable's slot is
/// SWidget::SetVisibility.
bool Settable(std::uintptr_t canvas) noexcept
{
    std::uintptr_t vtable   = 0;
    std::uintptr_t setter   = 0;
    std::uint64_t  delegate = 0;
    if (!Read(canvas, vtable) || !Read(vtable + kSetVisibilitySlot * sizeof(void*), setter) ||
        !Read(canvas + kVisibilityDelegate, delegate))
    {
        return false;
    }
    if (setter != reinterpret_cast<std::uintptr_t>(g_setVisibility) || delegate != 0)
    {
        if (!g_boundLogged)
        {
            g_boundLogged = true;
            platform::Log(L"area icons: a canvas is not the kind the research read (bound visibility or another "
                          L"setter); it is left shown in every view");
        }
        return false;
    }
    return true;
}

std::uint8_t VisibilityOf(std::uintptr_t canvas) noexcept
{
    std::uint8_t value = kSelfHitTestInvisible;
    Read(canvas + kVisibility, value);
    return value;
}

void SetCanvas(std::uintptr_t canvas, std::uint8_t value) noexcept
{
    if (canvas != 0 && VisibilityOf(canvas) != value && Settable(canvas))
    {
        SetVisibilityGuarded(canvas, value);
    }
}

bool EnsureInstance(int seat) noexcept
{
    if (g_instance[seat] != 0 || g_createFailed[seat])
    {
        return g_instance[seat] != 0;
    }
    g_instance[seat]     = CreateGuarded();
    g_createFailed[seat] = g_instance[seat] == 0;
    platform::Log(L"area icons: {}'s icon manager {}", players::SeatName(seat),
                  g_createFailed[seat] ? L"could not be created; they have none" : L"created with the game's constructor");
    return g_instance[seat] != 0;
}

/// Player 1's event state given to the seat's instance once, when the seat's ticks begin (at each
/// join): events from before are not replayed. From then on the replays keep it (FollowEvent).
void SeedEventState(int seat, std::uintptr_t one) noexcept
{
    const std::uintptr_t mine       = g_instance[seat];
    std::uint8_t         enabledOne = 0, enabledMine = 0, flag = 0;
    if (!Read(one + kEnabled, enabledOne) || !Read(mine + kEnabled, enabledMine) || !Read(one + kFlagD1, flag))
    {
        return;
    }
    platform::WriteGuarded(reinterpret_cast<void*>(mine + kFlagD1), &flag, sizeof(flag));
    if (enabledOne != 0 && enabledMine == 0)
    {
        const std::uint8_t on = 1;
        platform::WriteGuarded(reinterpret_cast<void*>(mine + kEnabled), &on, sizeof(on));
    }
    else if (enabledOne == 0 && enabledMine != 0)
    {
        ClearAllGuarded(mine);
    }
    g_seeded[seat] = true;
    platform::Log(L"area icons: {}'s icon manager starts with player 1's event state (enabled {}, flag {}); the "
                  L"game's events are followed from here{}",
                  players::SeatName(seat), enabledOne, flag,
                  g_eventsFollowed ? L"" : L" (NOT: their call sites are not redirected)");
}

/// After the game ran `event` on player 1's instance: its instance effect on each seat's, with the
/// game's own clear for a hide. Game thread only (the clear's counts are not atomic).
void FollowEvent(Event event, char flag) noexcept
{
    if (!players::generation::OnGameThread())
    {
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::Log(L"area icons: a map-icon event ran off the game thread; not followed for players 2 and 3 "
                          L"(logged once)");
        }
        return;
    }
    bool followed = false;
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        const std::uintptr_t mine = g_instance[seat];
        if (mine == 0)
        {
            continue;  // made at the seat's first tick, then seeded from player 1's
        }
        const std::uint8_t on = 1;
        switch (event)
        {
            case Event::Enable: platform::WriteGuarded(reinterpret_cast<void*>(mine + kEnabled), &on, sizeof(on)); break;
            case Event::HideAndClear: ClearAllGuarded(mine); break;
            case Event::SetFlagD1:
                platform::WriteGuarded(reinterpret_cast<void*>(mine + kFlagD1), &flag, sizeof(flag));
                break;
        }
        followed = true;
    }
    if (!followed)
    {
        return;
    }
    ++g_replays;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"area icons: the game's {} followed on players 2 and 3's icon managers ({} so far)",
                      event == Event::Enable         ? L"Enable"
                      : event == Event::HideAndClear ? L"HideAndClear"
                                                     : (flag != 0 ? L"SetFlagD1(1)" : L"SetFlagD1(0)"),
                      g_replays);
    }
}

// Each replaces the event at its call sites (a call, or a tail jump whose `ret` goes on to the
// caller's caller, as the event's would). Entered from the game's code: nothing here may throw.
void EnableSite() noexcept
{
    g_enable();
    FollowEvent(Event::Enable, 0);
}

void HideAndClearSite() noexcept
{
    g_hideAndClear();
    FollowEvent(Event::HideAndClear, 0);
}

void SetFlagD1Site(char flag) noexcept
{
    g_setFlagD1(flag);
    FollowEvent(Event::SetFlagD1, flag);
}

/// Puts the event sites back, the last installed first: two of them share one 16-byte block
/// (0x143544CB0, 0x143544CB9), and each redirect restores the block exactly as it found it, so only
/// the reverse of the install order finds each block as its redirect left it.
void RestoreEventSites() noexcept
{
    for (auto call = g_eventCalls.rbegin(); call != g_eventCalls.rend(); ++call)
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"area icons: a map-icon event site was changed after us; left in place (it still calls the "
                          L"event)");
        }
    }
}

/// Redirects all seven sites, or none.
bool FollowEvents(const platform::GameImage& image) noexcept
{
    const auto enable = platform::FindFunction(image, kEnable);
    const auto hide   = platform::FindFunction(image, kHideAndClear);
    const auto flag   = platform::FindFunction(image, kSetFlagD1);
    if (!enable || !hide || !flag)
    {
        return false;
    }
    g_enable       = reinterpret_cast<EventFn>(*enable);
    g_hideAndClear = reinterpret_cast<EventFn>(*hide);
    g_setFlagD1    = reinterpret_cast<SetFlagFn>(*flag);
    for (std::size_t i = 0; i < std::size(kEventSites); ++i)
    {
        const EventSite& site = kEventSites[i];
        const auto       live = image.Rebase(site.site, 5);
        const std::uintptr_t target = site.event == Event::Enable ? *enable : site.event == Event::HideAndClear ? *hide : *flag;
        const std::uintptr_t with =
            site.event == Event::Enable         ? reinterpret_cast<std::uintptr_t>(&EnableSite)
            : site.event == Event::HideAndClear ? reinterpret_cast<std::uintptr_t>(&HideAndClearSite)
                                                : reinterpret_cast<std::uintptr_t>(&SetFlagD1Site);
        const bool placed = live && (site.tailJump ? g_eventCalls[i].InstallTailJump(*live, target, with)
                                                   : g_eventCalls[i].Install(*live, target, with));
        if (!placed)
        {
            platform::Log(L"area icons: the map-icon event site at 0x{:X} is not what the research read", site.site);
            RestoreEventSites();
            return false;
        }
    }
    return true;
}
/// One line whenever player 1's or the seat's instance's enable flag, canvases or map sizes change.
void LogIfChanged(int seat, std::uintptr_t one) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    std::array<std::uint64_t, 7> now{};
    std::uint8_t  enabled[2]{};
    std::int32_t  slots[2][2]{};
    std::uintptr_t canvases[2][2]{};
    const std::uintptr_t instances[2] = {one, g_instance[seat]};
    for (int i = 0; i < 2; ++i)
    {
        Read(instances[i] + kEnabled, enabled[i]);
        for (int k = 0; k < 2; ++k)
        {
            Read(instances[i] + kMaps[k] + 8, slots[i][k]);  // the map's element array count
            canvases[i][k] = Canvas(instances[i], kCanvases[k]);
        }
    }
    now = {enabled[0] | (std::uint64_t{enabled[1]} << 8), static_cast<std::uint32_t>(slots[0][0]),
           static_cast<std::uint32_t>(slots[0][1]), static_cast<std::uint32_t>(slots[1][0]),
           static_cast<std::uint32_t>(slots[1][1]), canvases[1][0], canvases[1][1]};
    if (now == g_logged[seat])
    {
        return;
    }
    g_logged[seat] = now;
    try
    {
        platform::LogDebug(L"area icons: player 1's enabled {} icons/markers slots {}/{}; {}'s enabled {} slots {}/{} "
                      L"canvases {:#x}/{:#x}; first-controller answers {}, range-origin answers {}",
                      enabled[0], slots[0][0], slots[0][1], players::SeatName(seat), enabled[1], slots[1][0],
                      slots[1][1], canvases[1][0], canvases[1][1], g_answered, g_leaderAnswers);
    }
    catch (...)
    {
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto instance   = image.Rebase(kInstanceOne, sizeof(void*));
    const auto allocate   = platform::FindFunction(image, facts::kMalloc);
    const auto ctor       = platform::FindFunction(image, kCtor);
    const auto update     = platform::FindFunction(image, kFrameUpdate);
    const auto clear      = platform::FindFunction(image, kClearAll);
    const auto visibility = platform::FindFunction(image, kSetVisibility);
    const auto site       = image.Rebase(kFirstControllerCall, 5);
    const auto first      = platform::FindFunction(image, facts::kGetFirstPlayerController);
    if (!instance || !allocate || !ctor || !update || !clear || !visibility || !site || !first ||
        !g_firstControllerCall.Install(*site, *first, reinterpret_cast<std::uintptr_t>(&FirstController)))
    {
        platform::Log(L"area icons: the icon manager's code is not what the research read; no icons of players 2 "
                      L"and 3's own");
        return false;
    }
    const auto leaderSite = image.Rebase(kLeaderCall, 5);
    const auto leader     = platform::FindFunction(image, kLeader);
    if (!leaderSite || !leader ||
        !g_leaderCall.Install(*leaderSite, *leader, reinterpret_cast<std::uintptr_t>(&Leader)))
    {
        platform::Log(L"area icons: the range-origin call is not what the research read; players 2 and 3's icons "
                      L"range from player 1's character");
    }
    g_instanceOne   = *instance;
    g_malloc        = reinterpret_cast<MallocFn>(*allocate);
    g_ctor          = reinterpret_cast<CtorFn>(*ctor);
    g_frameUpdate   = reinterpret_cast<FrameUpdateFn>(*update);
    g_clearAll      = reinterpret_cast<ClearAllFn>(*clear);
    g_setVisibility = reinterpret_cast<SetVisibilityFn>(*visibility);
    g_installed     = true;
    g_eventsFollowed = FollowEvents(image);
    platform::Log(L"area icons: ready (player 2's and 3's icon managers are each made when they first play); the game's "
                  L"map-icon events {}",
                  g_eventsFollowed ? L"are followed at their seven call sites"
                                   : L"are NOT followed (player 2's and 3's managers keep the state they started with)");
    return true;
}

void Remove() noexcept
{
    g_hiddenOne      = {};  // no Slate calls here: this may run off the game thread (AllOut restored them)
    g_seatController = nullptr;
    g_seatMember     = nullptr;
    RestoreEventSites();
    if (g_leaderCall.IsInstalled() && !g_leaderCall.Restore())
    {
        platform::Log(L"area icons: the range-origin call was changed after us; left in place (it passes through)");
    }
    if (g_firstControllerCall.IsInstalled() && !g_firstControllerCall.Restore())
    {
        platform::Log(L"area icons: the first-controller call was changed after us; left in place (it passes through)");
    }
    g_installed = false;
}

void Tick(int seat, float deltaTime) noexcept
{
    const std::uintptr_t one = g_installed && Acts(kPartTick) && players::SeatIndex(seat) && !g_tickFailed[seat]
                                   ? InstanceOne()
                                   : 0;
    if (one == 0 || !EnsureInstance(seat))
    {
        return;
    }
    if (!g_seeded[seat])
    {
        SeedEventState(seat, one);
    }
    g_seatController  = Acts(kPartController) ? players::generation::ControllerOf(seat) : nullptr;
    g_seatMember      = Acts(kPartLeader) ? players::generation::MemberOf(seat) : nullptr;
    const bool ticked = FrameUpdateGuarded(g_instance[seat], deltaTime);
    g_seatController  = nullptr;
    g_seatMember      = nullptr;
    if (!ticked)
    {
        g_tickFailed[seat] = true;
        platform::Log(L"area icons: {}'s icon manager raised an exception; it is no longer ticked",
                      players::SeatName(seat));
        return;
    }
    LogIfChanged(seat, one);
}

void ShowOnly(int seat) noexcept
{
    if (!g_installed || !Acts(kPartCanvases))
    {
        return;
    }
    const std::uintptr_t one = InstanceOne();
    for (std::size_t k = 0; k < kCanvases.size(); ++k)
    {
        // A hide still pending is undone first (a pass that did not finish), so a visibility this
        // module set is never recorded as the canvas's own. Only the canvas that was hidden gets it.
        if (g_hiddenOne[k].canvas != 0 && Canvas(one, kCanvases[k]) == g_hiddenOne[k].canvas)
        {
            SetCanvas(g_hiddenOne[k].canvas, g_hiddenOne[k].visibility);
        }
        g_hiddenOne[k] = {};
        for (int other = 1; other < kPlayers; ++other)
        {
            if (other != seat)
            {
                SetCanvas(Canvas(g_instance[other], kCanvases[k]), kHidden);  // every other seat's
            }
        }
        if (seat == 0)
        {
            continue;
        }
        const std::uintptr_t canvas = Canvas(one, kCanvases[k]);
        const std::uint8_t   own    = canvas != 0 ? VisibilityOf(canvas) : kHidden;
        if (own != kHidden)  // one the game itself hides stays as the game has it
        {
            g_hiddenOne[k] = {canvas, own};
            SetCanvas(canvas, kHidden);
        }
        if (players::SeatIndex(seat))
        {
            SetCanvas(Canvas(g_instance[seat], kCanvases[k]), kSelfHitTestInvisible);
        }
    }
}

void SeatOut(int seat) noexcept
{
    if (!players::SeatIndex(seat) || g_instance[seat] == 0)
    {
        return;
    }
    ClearAllGuarded(g_instance[seat]);
    g_seeded[seat] = false;  // the next join starts from player 1's state again
    for (const std::uint32_t offset : kCanvases)
    {
        SetCanvas(Canvas(g_instance[seat], offset), kHidden);
    }
    platform::Log(L"area icons: {} is out; their icon manager cleared ({} game events followed)",
                  players::SeatName(seat), g_replays);
}

void AllOut() noexcept
{
    ShowOnly(0);
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        SeatOut(seat);
    }
}
void SetMask(unsigned mask) noexcept
{
    if (Acts(kPartCanvases) && (mask & kPartCanvases) == 0)
    {
        ShowOnly(0);  // player 1's canvases shown, as the game has them
        for (int seat = 1; seat < kPlayers; ++seat)
        {
            for (const std::uint32_t offset : kCanvases)
            {
                SetCanvas(Canvas(g_instance[seat], offset), kSelfHitTestInvisible);  // and each seat's, in every view
            }
        }
    }
    g_mask  = mask;
    platform::Log(L"area icons: mask {} (ticked {}, first controller the player's own {}, canvases per pass {}, ranged from "
                  L"the player's member {})",
                  mask, Acts(kPartTick), Acts(kPartController), Acts(kPartCanvases), Acts(kPartLeader));
}

unsigned Mask() noexcept
{
    return g_mask;
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"icons-mask",
                       .shape = control::Shape::Integer,
                       .max   = 15,
                       .help  = L"diagnosis: which parts of player 2's and 3's own map icons act (1 their icon "
                                L"manager ticked, 2 its first controller theirs, 4 each view shows its own canvases, 8 "
                                L"ranged from their member)",
                       .set   = [](const Value& v) {
                           SetMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"icons mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Mask()}}; }});
}

#endif

}  // namespace lss::hud::seat_area_icons
