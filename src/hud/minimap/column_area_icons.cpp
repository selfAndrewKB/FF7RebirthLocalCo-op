#include "hud/minimap/column_area_icons.h"

#include "facts/engine.h"
#include "facts/hud.h"
#include "hud/widget_visibility.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::column_area_icons {

namespace {

// Facts read from the database (research/RE-p2-hud-icon-manager.md; this build). Each
// function's prologue up to its stack probe call is checked before it is called or redirected.
constexpr std::uintptr_t kInstanceOne = 0x1491862F0;  // the game's instance (EndUISystem+0x1A0), a pointer
constexpr std::size_t    kInstanceSize = 0x160;

constexpr std::uint8_t   kCtorPrologue[]   = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// AreaMapIconMgr__Ctor(memory), returns it
constexpr platform::FunctionFact kCtor{L"AreaMapIconMgr__Ctor", 0x14374F5F4, kCtorPrologue};
constexpr std::uint8_t   kFrameUpdatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41,
                                                   0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x80, 0x07, 0x00, 0x00};
// AreaMapIcon_FrameUpdate(instance, xmm1 = DeltaTime)
constexpr platform::FunctionFact kFrameUpdate{L"AreaMapIcon_FrameUpdate", 0x1409B5D90, kFrameUpdatePrologue};
constexpr std::uint8_t   kClearAllPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
                                                0x89, 0x78, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1};
// AreaMapIconMgr__ClearAllAndDisable(instance)
constexpr platform::FunctionFact kClearAll{L"AreaMapIconMgr__ClearAllAndDisable", 0x140F62A48, kClearAllPrologue};

// The frame function's only call for the first player controller: E8 rel32 to
// UWorld__GetFirstPlayerController, rcx = the world, inside one 16-byte block.
constexpr std::uintptr_t kFirstControllerCall = 0x1409B5F10;

// The frame function's call for the range origin, the leader character by the leader's player type
// (research/RE-p2-hud-targets.md): E8 rel32 to Party_GetLeaderCharacterByLeaderType,
// no arguments, inside one 16-byte block.
constexpr std::uintptr_t kLeaderCall = 0x1409B5EB8;
constexpr std::uint8_t   kLeaderPrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0xD5, 0xA9, 0x00};
constexpr platform::FunctionFact kLeader{L"Party_GetLeaderCharacterByLeaderType", 0x1416CE5FC, kLeaderPrologue};

// The three game events that write player 1's instance, and their seven call sites
// (research/RE-p2-hud-view-levers.md). None takes the instance: each reads EndUISystem+0x1A0.
// Each site keeps calling the event, then the event's instance effect is applied to each column's
// instance directly. The events themselves are not run again: Enable and HideAndClear also play
// the in/out animation of one shared widget (EndUISystem+0x60), which a second run would restart.
constexpr std::uint8_t   kEnablePrologue[]    = {0xB8, 0x28, 0x00, 0x00, 0x00};
// void(): +0xD0 = 1, and the shared widget's "in"
constexpr platform::FunctionFact kEnable{L"AreaMapIconMgr Enable", 0x140F62978, kEnablePrologue};
constexpr std::uint8_t   kHideAndClearPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// void(): ClearAllAndDisable, and the widget's "out"
constexpr platform::FunctionFact kHideAndClear{L"AreaMapIconMgr HideAndClear", 0x140F6293C, kHideAndClearPrologue};
constexpr std::uint8_t   kSetFlagD1Prologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
// void(char): +0xD1 = cl
constexpr platform::FunctionFact kSetFlagD1{L"AreaMapIconMgr SetFlagD1", 0x1437123CC, kSetFlagD1Prologue};

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
    {0x1410D2407, Event::HideAndClear, false},  // EventCmd_Dispatch_BattleModeCase87
    {0x1410D2411, Event::Enable, false},        // the same event command
    {0x143273DD8, Event::Enable, true},         // EndUI__AreaMapIcon_EnableOrHideByCount, count > 0
    {0x143273DE1, Event::HideAndClear, true},   // the same, count <= 0
    {0x143290977, Event::SetFlagD1, false},     // a state's enter (cl = 0)
    {0x143293005, Event::SetFlagD1, false},     // that state's exit (cl = 1)
    {0x143299B60, Event::SetFlagD1, false},     // a stop/cleanup (cl = 1)
};

// The instance: the canvases as TWeakPtr {object, reference controller} (#1 icons, #2 quest
// and party markers), the icon and marker maps, and the fields game events write.
constexpr std::array<std::uint32_t, 2> kCanvases{0x10, 0x70};
constexpr std::array<std::uint32_t, 2> kMaps{0x20, 0x80};
constexpr std::uint32_t kEnabled   = 0xD0;  // u8
constexpr std::uint32_t kFlagD1    = 0xD1;  // u8

// The canvases' visibility values (hud/widget_visibility.h sets them as the game does).
constexpr std::uint8_t kHidden               = facts::kVisibilityHidden;
constexpr std::uint8_t kSelfHitTestInvisible = facts::kVisibilitySelfHitTestInvisible;  // what the canvases are made with


enum Part : unsigned
{
    kPartTick       = 1,
    kPartController = 2,
    kPartCanvases   = 4,
    kPartLeader     = 8,  // a column's icons ranged from the shown player's member
};

struct ShownCanvas  // a canvas of player 1's hidden for a further column's pass, and its own visibility
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
using EventFn         = void (*)();
using SetFlagFn       = void (*)(char flag);
using SetRenderTransformFn = bool (*)(void* widget, void* attribute, const void* value, std::uint8_t reason);

/// A widget's render-transform attribute as the game's setter takes it (facts/hud.h): set, with no getter delegate.
struct RenderTransform
{
    float         m00 = 1.0f, m01 = 0.0f, m10 = 0.0f, m11 = 1.0f;  // the 2x2 matrix
    float         tx = 0.0f, ty = 0.0f;                            // the translation
    std::uint8_t  optionalSet  = 1;
    std::uint8_t  pad19[3]{};
    std::uint8_t  attributeSet = 1;
    std::uint8_t  pad1D[3]{};
    void*         delegate      = nullptr;
    std::int32_t  delegateCount = 0;
    std::uint8_t  pad2C[4]{};
};
static_assert(sizeof(RenderTransform) == facts::kRenderTransformSize &&
                  offsetof(RenderTransform, optionalSet) == facts::kRenderTransformOptSet &&
                  offsetof(RenderTransform, attributeSet) == facts::kRenderTransformAttrSet,
              "the attribute as the game's setter reads it");

// Set once at install.
platform::CallRedirect g_firstControllerCall;
platform::CallRedirect g_leaderCall;
bool                   g_installed      = false;
std::uintptr_t         g_instanceOne    = 0;  // the address holding the game's instance
MallocFn               g_malloc         = nullptr;
CtorFn                 g_ctor           = nullptr;
FrameUpdateFn          g_frameUpdate    = nullptr;
ClearAllFn             g_clearAll       = nullptr;
EventFn                g_enable         = nullptr;
EventFn                g_hideAndClear   = nullptr;
SetFlagFn              g_setFlagD1      = nullptr;
SetRenderTransformFn   g_setTransform   = nullptr;  // null: diamonds keep the game's size
std::uintptr_t         g_questVTable    = 0;        // the field diamond widget's vtable, live
std::array<platform::CallRedirect, std::size(kEventSites)> g_eventCalls;
bool                   g_eventsFollowed = false;

constexpr int kColumns = views::columns::kMaxColumns;

// Game thread. By column ([0] unused: player 1's is the game's): each further column's own instance, made with the
// game's constructor and kept.
std::uintptr_t               g_instance[kColumns]     = {};
bool                         g_createFailed[kColumns] = {};
bool                         g_tickFailed[kColumns]   = {};
bool                         g_seeded[kColumns]       = {};  // the column's instance has player 1's event state
// The controller id each column's instance was last placed for (-1: none yet; 0 would be player 1's).
std::array<std::int32_t, kColumns> g_ownerId = [] {
    std::array<std::int32_t, kColumns> ids{};
    ids.fill(-1);
    return ids;
}();
std::array<std::uint64_t, 7> g_logged[kColumns]       = {};
void*                        g_columnController = nullptr;  // answered while a column's tick runs
void*                        g_columnMember     = nullptr;  // answered as the range origin while it runs
unsigned                     g_mask   = kPartTick | kPartController | kPartCanvases | kPartLeader;
std::array<ShownCanvas, 2>   g_hiddenOne{};
std::uint32_t                g_answered = 0;  // first-controller calls answered with a column's
std::uint32_t                g_leaderAnswers = 0;  // range-origin calls answered with a shown player's member
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
    if (g_columnController != nullptr)
    {
        ++g_answered;
        return g_columnController;
    }
    return reinterpret_cast<FirstControllerFn>(g_firstControllerCall.Original())(world);
}

/// Replaces the frame function's call for the range origin: the shown player's member in a column's tick.
void* Leader() noexcept
{
    if (g_columnMember != nullptr)
    {
        ++g_leaderAnswers;
        return g_columnMember;
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

std::uintptr_t InstanceOne() noexcept
{
    std::uintptr_t instance = 0;
    Read(g_instanceOne, instance);
    return instance;
}

/// The canvas a weak pointer at `instance + offset` holds, or 0 if it does not pin.
std::uintptr_t Canvas(std::uintptr_t instance, std::uint32_t offset) noexcept
{
    return instance != 0 ? widget_visibility::PinnedWidget(instance + offset) : 0;
}

std::uint8_t VisibilityOf(std::uintptr_t canvas) noexcept
{
    return widget_visibility::ValueOf(canvas, kSelfHitTestInvisible);
}

/// The canvas's visibility set as the game sets it; a canvas of another kind (bound visibility, another setter) is left
/// shown in every view (logged once).
void SetCanvas(std::uintptr_t canvas, std::uint8_t value) noexcept
{
    if (canvas == 0 || VisibilityOf(canvas) == value)
    {
        return;
    }
    if (!widget_visibility::Settable(canvas))
    {
        if (!g_boundLogged)
        {
            g_boundLogged = true;
            platform::Log(L"area icons: a canvas is not the kind the research read (bound visibility or another "
                          L"setter); it is left shown in every view");
        }
        return;
    }
    widget_visibility::Set(canvas, value);
}

bool EnsureInstance(int column) noexcept
{
    if (g_instance[column] != 0 || g_createFailed[column])
    {
        return g_instance[column] != 0;
    }
    g_instance[column]     = CreateGuarded();
    g_createFailed[column] = g_instance[column] == 0;
    platform::Log(L"area icons: column {}'s icon manager {}", column + 1,
                  g_createFailed[column] ? L"could not be created; it has none" : L"created with the game's constructor");
    return g_instance[column] != 0;
}

/// The column's instance cleared with the game's own clear, its canvases hidden; the next tick seeds it again.
void ClearColumn(int column) noexcept
{
    const std::uintptr_t mine = g_instance[column];
    g_seeded[column]          = false;
    if (mine == 0)
    {
        return;
    }
    ClearAllGuarded(mine);
    for (const std::uint32_t offset : kCanvases)
    {
        SetCanvas(Canvas(mine, offset), kHidden);
    }
}

/// Player 1's event state given to the column's instance once, when its ticks begin (each time the column changes
/// hands): events from before are not replayed. From then on the replays keep it (FollowEvent).
void SeedEventState(int column, std::uintptr_t one) noexcept
{
    const std::uintptr_t mine       = g_instance[column];
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
    g_seeded[column] = true;
    platform::Log(L"area icons: column {}'s icon manager starts with player 1's event state (enabled {}, flag {}); the "
                  L"game's events are followed from here{}",
                  column + 1, enabledOne, flag,
                  g_eventsFollowed ? L"" : L" (NOT: their call sites are not redirected)");
}

/// After the game ran `event` on player 1's instance: its instance effect on each column's, with the
/// game's own clear for a hide. Game thread only (the clear's counts are not atomic).
void FollowEvent(Event event, char flag) noexcept
{
    if (!players::generation::OnGameThread())
    {
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::Log(L"area icons: a map-icon event ran off the game thread; not followed for the further columns "
                          L"(logged once)");
        }
        return;
    }
    bool followed = false;
    for (int column = 1; column < kColumns; ++column)
    {
        const std::uintptr_t mine = g_instance[column];
        if (mine == 0)
        {
            continue;  // made at the column's first tick, then seeded from player 1's
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
        platform::LogDebug(L"area icons: the game's {} followed on the further columns' icon managers ({} so far)",
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

/// Puts the event sites back, the last installed first: two of them share one 16-byte block,
/// and each redirect restores the block exactly as it found it, so only
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
/// One line whenever player 1's or the column's instance's enable flag, canvases or map sizes change.
void LogIfChanged(int column, std::uintptr_t one) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    std::array<std::uint64_t, 7> now{};
    std::uint8_t  enabled[2]{};
    std::int32_t  slots[2][2]{};
    std::uintptr_t canvases[2][2]{};
    const std::uintptr_t instances[2] = {one, g_instance[column]};
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
    if (now == g_logged[column])
    {
        return;
    }
    g_logged[column] = now;
    try
    {
        platform::LogDebug(L"area icons: player 1's enabled {} icons/markers slots {}/{}; column {}'s enabled {} slots "
                      L"{}/{} canvases {:#x}/{:#x}; first-controller answers {}, range-origin answers {}",
                      enabled[0], slots[0][0], slots[0][1], column + 1, enabled[1], slots[1][0],
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
    const bool visibility = widget_visibility::Installed() || widget_visibility::Install(image);
    const auto site       = image.Rebase(kFirstControllerCall, 5);
    const auto first      = platform::FindFunction(image, facts::kGetFirstPlayerController);
    if (!instance || !allocate || !ctor || !update || !clear || !visibility || !site || !first ||
        !g_firstControllerCall.Install(*site, *first, reinterpret_cast<std::uintptr_t>(&FirstController)))
    {
        platform::Log(L"area icons: the icon manager's code is not what the research read; the further columns have "
                      L"no icons of their own");
        return false;
    }
    const auto leaderSite = image.Rebase(kLeaderCall, 5);
    const auto leader     = platform::FindFunction(image, kLeader);
    if (!leaderSite || !leader ||
        !g_leaderCall.Install(*leaderSite, *leader, reinterpret_cast<std::uintptr_t>(&Leader)))
    {
        platform::Log(L"area icons: the range-origin call is not what the research read; the further columns' icons "
                      L"range from player 1's character");
    }
    g_instanceOne   = *instance;
    g_malloc        = reinterpret_cast<MallocFn>(*allocate);
    g_ctor          = reinterpret_cast<CtorFn>(*ctor);
    g_frameUpdate   = reinterpret_cast<FrameUpdateFn>(*update);
    g_clearAll      = reinterpret_cast<ClearAllFn>(*clear);
    g_installed     = true;
    g_eventsFollowed = FollowEvents(image);
    const auto transform = platform::FindFunction(image, facts::kSetRenderTransform);
    const auto vtable    = image.Rebase(facts::kFieldQuestIconVTable, sizeof(void*));
    if (transform && vtable)
    {
        g_setTransform    = reinterpret_cast<SetRenderTransformFn>(*transform);
        g_questVTable     = *vtable;
    }
    else
    {
        platform::Log(L"area icons: the diamond's render-transform setter or its widget's vtable was not found; a player's "
                      L"own lead marker keeps the game's size");
    }
    platform::Log(L"area icons: ready (each further column's icon manager is made at its first pass); the game's "
                  L"map-icon events {}",
                  g_eventsFollowed ? L"are followed at their seven call sites"
                                   : L"are NOT followed (the columns' managers keep the state they started with)");
    return true;
}

void Remove() noexcept
{
    g_hiddenOne        = {};  // no Slate calls here: this may run off the game thread (AllOut restored them)
    g_columnController = nullptr;
    g_columnMember     = nullptr;
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

void FollowColumns(const views::columns::Map& map) noexcept
{
    for (int column = 1; column < kColumns; ++column)
    {
        const auto         c  = static_cast<std::size_t>(column);
        const std::int32_t id = map.idAt[c];
        if (id == g_ownerId[c])
        {
            continue;
        }
        const bool had = g_instance[column] != 0 && g_seeded[column];
        g_ownerId[c]   = id;
        ClearColumn(column);
        if (had)
        {
            platform::Log(L"area icons: column {} {}; its icon manager cleared ({} game events followed)", column + 1,
                          id < 0 ? L"is gone" : L"changed hands", g_replays);
        }
    }
}

void Tick(int column, void* controller, void* member, float deltaTime) noexcept
{
    const bool           further = column >= 1 && column < kColumns;
    const std::uintptr_t one     = g_installed && Acts(kPartTick) && further && !g_tickFailed[column] ? InstanceOne() : 0;
    if (one == 0 || !EnsureInstance(column))
    {
        return;
    }
    if (!g_seeded[column])
    {
        SeedEventState(column, one);
    }
    g_columnController = Acts(kPartController) ? controller : nullptr;
    g_columnMember     = Acts(kPartLeader) ? member : nullptr;
    const bool ticked  = FrameUpdateGuarded(g_instance[column], deltaTime);
    g_columnController = nullptr;
    g_columnMember     = nullptr;
    if (!ticked)
    {
        g_tickFailed[column] = true;
        platform::Log(L"area icons: column {}'s icon manager raised an exception; it is no longer ticked", column + 1);
        return;
    }
    LogIfChanged(column, one);
}

namespace {

/// The live widget of `key` in the column's instance (0: player 1's, the game's), or 0.
std::uintptr_t IconWidget(int column, std::uint64_t key) noexcept
{
    if (!g_installed || key == 0 || column < 0 || column >= kColumns)
    {
        return 0;
    }
    const std::uintptr_t instance = column == 0 ? InstanceOne() : g_instance[column];
    std::uintptr_t       widget   = 0;
    try
    {
        platform::ForEachSparseElement(instance != 0 ? instance + facts::kAreaIconMap : 0, facts::kAreaIconMapElement,
                                       [&](std::uintptr_t element) {
                                           std::uint64_t  name  = 0;
                                           std::uintptr_t value = 0;
                                           if (!Read(element, name) || name != key)
                                           {
                                               return true;
                                           }
                                           if (Read(element + facts::kAreaIconMapValue, value) && value != 0)
                                           {
                                               widget = widget_visibility::PinnedWidget(value);
                                           }
                                           return false;
                                       });
    }
    catch (...)
    {
        return 0;
    }
    return widget;
}

}  // namespace

bool HideIcon(int column, std::uint64_t key) noexcept
{
    const std::uintptr_t widget = IconWidget(column, key);
    return widget != 0 && widget_visibility::Set(widget, kHidden);
}

bool ScaleIcon(int column, std::uint64_t key, float scale) noexcept
{
    if (g_setTransform == nullptr || column < 1)
    {
        return false;
    }
    const std::uintptr_t widget = IconWidget(column, key);
    std::uintptr_t       vtable = 0, image = 0;
    if (widget == 0 || !Read(widget, vtable) || vtable != g_questVTable || !Read(widget + facts::kIconImage, image) ||
        image == 0)
    {
        return false;
    }
    // The image grows about its centre (the game's pivot): lifted by the growth below the centre, its bottom stays on
    // the line.
    RenderTransform value;
    value.m00 = scale;
    value.m11 = scale;
    value.ty  = -(scale - 1.0f) * facts::kQuestIconSize / 2.0f;
    void* const target = reinterpret_cast<void*>(image);
    void* const field  = reinterpret_cast<void*>(image + facts::kWidgetRenderTransform);
    bool        changed = false;  // the setter's answer (false: already this value); only the guard's result matters
    return platform::CallGuarded(g_setTransform, changed, target, field, static_cast<const void*>(&value),
                                 facts::kRenderTransformReason);
}

void ShowOnly(int column) noexcept
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
        for (int other = 1; other < kColumns; ++other)
        {
            if (other != column)
            {
                SetCanvas(Canvas(g_instance[other], kCanvases[k]), kHidden);  // every other column's
            }
        }
        if (column <= 0 || column >= kColumns)
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
        SetCanvas(Canvas(g_instance[column], kCanvases[k]), kSelfHitTestInvisible);
    }
}

void AllOut() noexcept
{
    ShowOnly(0);
}

void SetMask(unsigned mask) noexcept
{
    if (Acts(kPartCanvases) && (mask & kPartCanvases) == 0)
    {
        ShowOnly(0);  // player 1's canvases shown, as the game has them
        for (int column = 1; column < kColumns; ++column)
        {
            for (const std::uint32_t offset : kCanvases)
            {
                SetCanvas(Canvas(g_instance[column], offset), kSelfHitTestInvisible);  // and each column's, in every view
            }
        }
    }
    g_mask  = mask;
    platform::Log(L"area icons: mask {} (ticked {}, first controller the column's {}, canvases per pass {}, ranged from "
                  L"the shown player's member {})",
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
                       .help  = L"diagnosis: which parts of the further columns' own map icons act (1 each column's "
                                L"icon manager ticked, 2 its first controller the column's, 4 each view shows its own "
                                L"canvases, 8 ranged from the shown player's member)",
                       .set   = [](const Value& v) {
                           SetMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"icons mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Mask()}}; }});
}

#endif

}  // namespace lss::hud::column_area_icons
