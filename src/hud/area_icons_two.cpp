#include "hud/area_icons_two.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <array>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::area_icons_two {

namespace {

// Facts read from the database (RE-p2-hud-icon-manager.md §1, §4 and §8; this build). Each
// function's prologue up to its stack probe call is checked before it is called or redirected.
constexpr std::uintptr_t kInstanceOne = 0x149349880;  // the game's instance (EndUISystem+0x1A0), a pointer
constexpr std::size_t    kInstanceSize = 0x160;

constexpr std::uintptr_t kMalloc           = 0x14083F3D8;  // FMemory_Malloc(rcx = size); never reads rdx
constexpr std::uint8_t   kMallocPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kCtor             = 0x143A0D944;  // AreaMapIconMgr__Ctor(memory), returns it
constexpr std::uint8_t   kCtorPrologue[]   = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFrameUpdate      = 0x1409F72B8;  // AreaMapIcon_FrameUpdate(instance, xmm1 = DeltaTime)
constexpr std::uint8_t   kFrameUpdatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41,
                                                   0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x50, 0x07, 0x00, 0x00};
constexpr std::uintptr_t kClearAll         = 0x140F5D854;  // AreaMapIconMgr__ClearAllAndDisable(instance)
constexpr std::uint8_t   kClearAllPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
                                                0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0xB8};

// The frame function's only call for the first player controller (§8.2): E8 rel32 at 0x1409F7439
// to UWorld__GetFirstPlayerController, rcx = the world, inside the block at 0x1409F7430.
constexpr std::uintptr_t kFirstControllerCall = 0x1409F7439;
constexpr std::uintptr_t kFirstController     = 0x140EF3078;
constexpr std::uint8_t   kFirstControllerPrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00};

// The frame function's call for the range origin, the leader character by the leader's player type
// (RE-p2-hud-targets.md §1.1, R1): E8 rel32 at 0x1409F73E4 to Party_GetLeaderCharacterByLeaderType,
// no arguments, inside the block at 0x1409F73E0.
constexpr std::uintptr_t kLeaderCall = 0x1409F73E4;
constexpr std::uintptr_t kLeader     = 0x140C81DE0;
constexpr std::uint8_t   kLeaderPrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x46, 0xD1, 0x42, 0x01};

// The three game events that write player 1's instance, and their seven call sites
// (RE-p2-hud-view-levers.md §6 and S-HUD31). None takes the instance: each reads EndUISystem+0x1A0.
// Each site keeps calling the event, then the event's instance effect is applied to player 2's
// instance directly. The events themselves are not run again: Enable and HideAndClear also play
// the in/out animation of one shared widget (EndUISystem+0x60), which a second run would restart.
constexpr std::uintptr_t kEnable              = 0x141816F1C;  // void(): +0xD0 = 1, and the shared widget's "in"
constexpr std::uint8_t   kEnablePrologue[]    = {0xB8, 0x28, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kHideAndClear        = 0x141816EE0;  // void(): ClearAllAndDisable, and the widget's "out"
constexpr std::uint8_t   kHideAndClearPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kSetFlagD1           = 0x1439D1CEC;  // void(char): +0xD1 = cl
constexpr std::uint8_t   kSetFlagD1Prologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};

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
constexpr std::uintptr_t kSetVisibility           = 0x140C27B38;
constexpr std::uint8_t   kSetVisibilityPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
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

constexpr int kMaxLines = 60;

enum Part : unsigned
{
    kPartTick       = 1,
    kPartController = 2,
    kPartCanvases   = 4,
    kPartLeader     = 8,  // player 2's icons ranged from player 2's member
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

struct ShownCanvas  // a canvas of player 1's hidden for player 2's pass, and its own visibility
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

// Game thread.
std::uintptr_t               g_instanceTwo = 0;
bool                         g_createFailed = false;
bool                         g_tickFailed   = false;
void*                        g_playerTwoController = nullptr;  // answered while player 2's tick runs
void*                        g_playerTwoMember     = nullptr;  // answered as the range origin while it runs
unsigned                     g_mask   = kPartTick | kPartController | kPartCanvases | kPartLeader;
std::array<ShownCanvas, 2>   g_hiddenOne{};
std::uint32_t                g_answered = 0;  // first-controller calls answered with player 2's
std::uint32_t                g_leaderAnswers = 0;  // range-origin calls answered with player 2's member
std::array<std::uint64_t, 7> g_logged{};
int                          g_lines = 0;
bool                         g_boundLogged = false;
bool                         g_seeded      = false;  // player 2's instance has player 1's event state
bool                         g_offThreadLogged = false;
std::uint32_t                g_replays     = 0;
int                          g_replayLines = 0;
constexpr int                kMaxReplayLines = 40;

bool Acts(Part part) noexcept
{
    return (g_mask & part) != 0;
}

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// Replaces the frame function's call for the first player controller.
void* FirstController(void* world) noexcept
{
    if (g_playerTwoController != nullptr)
    {
        ++g_answered;
        return g_playerTwoController;
    }
    return reinterpret_cast<FirstControllerFn>(g_firstControllerCall.Original())(world);
}

/// Replaces the frame function's call for the range origin: player 2's member in player 2's tick.
void* Leader() noexcept
{
    if (g_playerTwoMember != nullptr)
    {
        ++g_leaderAnswers;
        return g_playerTwoMember;
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
                          L"setter); it is left shown in both halves");
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

bool EnsureInstanceTwo() noexcept
{
    if (g_instanceTwo != 0 || g_createFailed)
    {
        return g_instanceTwo != 0;
    }
    g_instanceTwo = CreateGuarded();
    g_createFailed = g_instanceTwo == 0;
    platform::Log(L"area icons: player 2's icon manager {}", g_createFailed ? L"could not be created; none for player 2"
                                                                            : L"created with the game's constructor");
    return g_instanceTwo != 0;
}

/// Player 1's event state given to player 2's instance once, when player 2's ticks begin (at each
/// join): events from before are not replayed. From then on the replays keep it (FollowEvent).
void SeedEventState(std::uintptr_t one) noexcept
{
    std::uint8_t enabledOne = 0, enabledTwo = 0, flag = 0;
    if (!Read(one + kEnabled, enabledOne) || !Read(g_instanceTwo + kEnabled, enabledTwo) || !Read(one + kFlagD1, flag))
    {
        return;
    }
    platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kFlagD1), &flag, sizeof(flag));
    if (enabledOne != 0 && enabledTwo == 0)
    {
        const std::uint8_t on = 1;
        platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kEnabled), &on, sizeof(on));
    }
    else if (enabledOne == 0 && enabledTwo != 0)
    {
        ClearAllGuarded(g_instanceTwo);
    }
    g_seeded = true;
    platform::Log(L"area icons: player 2's icon manager starts with player 1's event state (enabled {}, flag {}); the "
                  L"game's events are followed from here{}",
                  enabledOne, flag, g_eventsFollowed ? L"" : L" (NOT: their call sites are not redirected)");
}

/// After the game ran `event` on player 1's instance: its instance effect on player 2's, with the
/// game's own clear for a hide. Game thread only (the clear's counts are not atomic).
void FollowEvent(Event event, char flag) noexcept
{
    if (!player_two::generation::OnGameThread())
    {
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::Log(L"area icons: a map-icon event ran off the game thread; not followed for player 2 (logged once)");
        }
        return;
    }
    if (g_instanceTwo == 0)
    {
        return;  // made at player 2's first tick, then seeded from player 1's
    }
    const std::uint8_t on = 1;
    switch (event)
    {
        case Event::Enable:
            platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kEnabled), &on, sizeof(on));
            break;
        case Event::HideAndClear: ClearAllGuarded(g_instanceTwo); break;
        case Event::SetFlagD1:
            platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kFlagD1), &flag, sizeof(flag));
            break;
    }
    ++g_replays;
    if (g_replayLines < kMaxReplayLines && platform::Enabled(platform::Level::Debug))
    {
        ++g_replayLines;
        platform::LogDebug(L"area icons: the game's {} followed on player 2's icon manager ({} so far)",
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
    const auto enable = image.Rebase(kEnable);
    const auto hide   = image.Rebase(kHideAndClear);
    const auto flag   = image.Rebase(kSetFlagD1);
    if (!enable || !hide || !flag || !image.BytesMatch(kEnable, kEnablePrologue) ||
        !image.BytesMatch(kHideAndClear, kHideAndClearPrologue) || !image.BytesMatch(kSetFlagD1, kSetFlagD1Prologue))
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
/// One line whenever either instance's enable flag, canvases or map sizes change.
void LogIfChanged(std::uintptr_t one) noexcept
{
    if (g_lines >= kMaxLines || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    std::array<std::uint64_t, 7> now{};
    std::uint8_t  enabled[2]{};
    std::int32_t  slots[2][2]{};
    std::uintptr_t canvases[2][2]{};
    const std::uintptr_t instances[2] = {one, g_instanceTwo};
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
    if (now == g_logged)
    {
        return;
    }
    g_logged = now;
    ++g_lines;
    try
    {
        platform::LogDebug(L"area icons: player 1's enabled {} icons/markers slots {}/{}; player 2's enabled {} slots {}/{} "
                      L"canvases {:#x}/{:#x}; first-controller answers {}, range-origin answers {}",
                      enabled[0], slots[0][0], slots[0][1], enabled[1], slots[1][0], slots[1][1], canvases[1][0],
                      canvases[1][1], g_answered, g_leaderAnswers);
    }
    catch (...)
    {
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool prologues = image.BytesMatch(kMalloc, kMallocPrologue) && image.BytesMatch(kCtor, kCtorPrologue) &&
                           image.BytesMatch(kFrameUpdate, kFrameUpdatePrologue) &&
                           image.BytesMatch(kClearAll, kClearAllPrologue) &&
                           image.BytesMatch(kFirstController, kFirstControllerPrologue) &&
                           image.BytesMatch(kSetVisibility, kSetVisibilityPrologue);
    const auto instance   = image.Rebase(kInstanceOne, sizeof(void*));
    const auto allocate   = image.Rebase(kMalloc);
    const auto ctor       = image.Rebase(kCtor);
    const auto update     = image.Rebase(kFrameUpdate);
    const auto clear      = image.Rebase(kClearAll);
    const auto visibility = image.Rebase(kSetVisibility);
    const auto site       = image.Rebase(kFirstControllerCall, 5);
    const auto first      = image.Rebase(kFirstController);
    if (!prologues || !instance || !allocate || !ctor || !update || !clear || !visibility || !site || !first ||
        !g_firstControllerCall.Install(*site, *first, reinterpret_cast<std::uintptr_t>(&FirstController)))
    {
        platform::Log(L"area icons: the icon manager's code is not what the research read; no icons of player 2's own");
        return false;
    }
    const auto leaderSite = image.Rebase(kLeaderCall, 5);
    const auto leader     = image.Rebase(kLeader);
    if (!leaderSite || !leader || !image.BytesMatch(kLeader, kLeaderPrologue) ||
        !g_leaderCall.Install(*leaderSite, *leader, reinterpret_cast<std::uintptr_t>(&Leader)))
    {
        platform::Log(L"area icons: the range-origin call is not what the research read; player 2's icons range from "
                      L"player 1's character");
    }
    g_instanceOne   = *instance;
    g_malloc        = reinterpret_cast<MallocFn>(*allocate);
    g_ctor          = reinterpret_cast<CtorFn>(*ctor);
    g_frameUpdate   = reinterpret_cast<FrameUpdateFn>(*update);
    g_clearAll      = reinterpret_cast<ClearAllFn>(*clear);
    g_setVisibility = reinterpret_cast<SetVisibilityFn>(*visibility);
    g_installed     = true;
    g_eventsFollowed = FollowEvents(image);
    platform::Log(L"area icons: ready (player 2's icon manager is made when player 2 first plays); the game's map-icon "
                  L"events {}",
                  g_eventsFollowed ? L"are followed at their seven call sites"
                                   : L"are NOT followed (player 2's manager keeps the state it started with)");
    return true;
}

void Remove() noexcept
{
    g_hiddenOne           = {};  // no Slate calls here: this may run off the game thread (PlayerTwoOut restored them)
    g_playerTwoController = nullptr;
    g_playerTwoMember     = nullptr;
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

void Tick(float deltaTime) noexcept
{
    const std::uintptr_t one = g_installed && Acts(kPartTick) && !g_tickFailed ? InstanceOne() : 0;
    if (one == 0 || !EnsureInstanceTwo())
    {
        return;
    }
    if (!g_seeded)
    {
        SeedEventState(one);
    }
    g_playerTwoController = Acts(kPartController) ? player_two::generation::PlayerTwoController() : nullptr;
    g_playerTwoMember     = Acts(kPartLeader) ? player_two::generation::Member() : nullptr;
    const bool ticked     = FrameUpdateGuarded(g_instanceTwo, deltaTime);
    g_playerTwoController = nullptr;
    g_playerTwoMember     = nullptr;
    if (!ticked)
    {
        g_tickFailed = true;
        platform::Log(L"area icons: player 2's icon manager raised an exception; it is no longer ticked");
        return;
    }
    LogIfChanged(one);
}

void ShowOnly(int player) noexcept
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
        g_hiddenOne[k]          = {};
        const std::uintptr_t two = Canvas(g_instanceTwo, kCanvases[k]);
        if (player == 0)
        {
            SetCanvas(two, kHidden);
            continue;
        }
        const std::uintptr_t canvas = Canvas(one, kCanvases[k]);
        const std::uint8_t   own    = canvas != 0 ? VisibilityOf(canvas) : kHidden;
        if (own != kHidden)  // one the game itself hides stays as the game has it
        {
            g_hiddenOne[k] = {canvas, own};
            SetCanvas(canvas, kHidden);
        }
        SetCanvas(two, kSelfHitTestInvisible);
    }
}

void PlayerTwoOut() noexcept
{
    ShowOnly(0);
    if (g_instanceTwo == 0)
    {
        return;
    }
    ClearAllGuarded(g_instanceTwo);
    g_seeded = false;  // the next join starts from player 1's state again
    for (const std::uint32_t offset : kCanvases)
    {
        SetCanvas(Canvas(g_instanceTwo, offset), kHidden);
    }
    platform::Log(L"area icons: player 2 is out; player 2's icon manager cleared ({} game events followed)", g_replays);
}

void SetMask(unsigned mask) noexcept
{
    if (Acts(kPartCanvases) && (mask & kPartCanvases) == 0)
    {
        ShowOnly(0);  // player 1's canvases shown, as the game has them
        for (const std::uint32_t offset : kCanvases)
        {
            SetCanvas(Canvas(g_instanceTwo, offset), kSelfHitTestInvisible);  // and player 2's, in both halves
        }
    }
    g_mask  = mask;
    g_lines = 0;
    platform::Log(L"area icons: mask {} (ticked {}, first controller player 2's {}, canvases per pass {}, ranged from "
                  L"player 2's member {})",
                  mask, Acts(kPartTick), Acts(kPartController), Acts(kPartCanvases), Acts(kPartLeader));
}

}  // namespace lss::hud::area_icons_two
