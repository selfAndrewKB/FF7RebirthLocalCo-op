#include "hud/player_two_pass.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "hud/first_player_code.h"
#include "hud/hud_addresses.h"
#include "hud/user_widgets.h"
#include "platform/code_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

using RC::Unreal::UObject;

// Facts read from the database (RE-p2-hud.md pass 9 table, pass 12). Each function's prologue up
// to its stack probe call is checked before it is called.
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;  // g_EndUISystem (data)
constexpr std::uintptr_t kGameMode    = 0x1490FF2B8;  // g_AEndGameMode (pointer)
constexpr std::uint32_t  kGameModeHudTimeStep = 0x5D8;  // float, written once per engine tick

struct SubTick
{
    std::uint32_t                 member;  // EndUISystem member holding the object (a pointer)
    std::uintptr_t                function;  // void (object)
    std::span<const std::uint8_t> prologue;
};

constexpr std::uint8_t kTick98[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41,
                                     0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0xB8, 0xC0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTickB0[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x40, 0x01, 0x00, 0x00};
constexpr std::uint8_t kTick150[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                     0x48, 0x8D, 0x6C, 0x24, 0xE1, 0xB8, 0xE8, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick188[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x56, 0xB8, 0xE0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick170[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78,
                                     0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0xB8, 0xA0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick180[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xB0, 0x01, 0x00, 0x00};

// In MasterHUD_Tick's order (pass 9, P9.1).
const std::array<SubTick, 6> kSubTicks{{
    {0x98, 0x140A677F0, kTick98},
    {0xB0, 0x140B8B374, kTickB0},
    {0x150, 0x140C8D360, kTick150},  // EndUI__MarkerList_Tick
    {0x188, 0x140F37874, kTick188},
    {0x170, 0x140F37ACC, kTick170},
    {0x180, 0x140A68620, kTick180},
}};

// The leader-target icon (pass 9 step 7): m = *(g+0x140); if *(int32*)(m+0x20) != 0 and
// w = FWeakObjectPtr::Get(m+0x6C) is set, 0x1417267F0(w).
constexpr std::uint32_t  kLeaderTargetMember = 0x140;
constexpr std::uint32_t  kLeaderTargetCount  = 0x20;
constexpr std::uint32_t  kLeaderTargetWeak   = 0x6C;
constexpr std::uintptr_t kWeakGet            = 0x141905998;
constexpr std::uint8_t   kWeakGetPrologue[]  = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F};
constexpr std::uintptr_t kLeaderTargetTick   = 0x1417267F0;
constexpr std::uint8_t   kLeaderTargetPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
                                                    0x89, 0x78, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0xB8, 0xA0, 0x00, 0x00, 0x00};

// The compass (pass 12 item 5): CompassHolder__FrameUpdate(holder, DeltaTime), holder =
// *(*(g+0x108)+0x348). Its heading and scale come from the primary camera manager (L3). Run with a
// DeltaTime of 0 for each player [pass 12: looks safe to re-run, not fully traced].
constexpr std::uint32_t  kCompassManager    = 0x108;
constexpr std::uint32_t  kCompassHolder     = 0x348;
constexpr std::uintptr_t kCompassFrameUpdate = 0x14122FBA0;
constexpr std::uint8_t   kCompassFrameUpdatePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
                                                          0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                          0x41, 0x57, 0x48, 0x8D, 0xA8, 0xC8, 0xFD, 0xFF, 0xFF, 0xB8, 0x10,
                                                          0x03, 0x00, 0x00};

// UUserWidget::SetPlayerContext(widget, const FLocalPlayerContext&): writes the 16 bytes at +0x248
// (weak local player, weak world) and walks the widget's child user widgets. No events.
constexpr std::uintptr_t kSetPlayerContext = 0x140F59C50;
constexpr std::uint8_t   kSetPlayerContextPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC,
                                                        0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr std::uint32_t  kWidgetWorldWeak     = 0x250;

// GEngine__GetFirstGamePlayerForGameViewport's first instruction (hud/first_player_code.h).
constexpr std::uintptr_t kFirstGamePlayer       = 0x140CCF8E4;
constexpr std::uintptr_t kFirstGamePlayerResume = 0x140CCF8E9;

// The overlay's cached player view rect (RE-p2-hud.md P2.5): min x/y and size, floats.
constexpr std::array<std::uint32_t, 4> kOverlayRectFields{0x318, 0x31C, 0x328, 0x32C};

// New roots (markers the game adds) get player 2's context within this many frames.
constexpr std::uint32_t kRootScanFrames = 30;
constexpr std::uint32_t kTimingFrames   = 240;

/// What each part of the pass costs, for the periodic log line.
enum Part : std::size_t
{
    kContextsTwo,
    kTicksTwo,
    kContextsOne,
    kTicksOne,
    kScan,
    kParts,
};
constexpr const wchar_t* kPartNames[kParts] = {L"contexts to player 2", L"updates for player 2", L"contexts to player 1",
                                               L"updates for player 1", L"root scan"};

using ObjectFn           = void (*)(void* object);
using CompassFn          = void (*)(void* holder, float deltaTime);
using WeakGetFn          = void* (*)(const void* weak);
using SetPlayerContextFn = void (*)(void* widget, const void* context);

struct alignas(8) PlayerContext
{
    std::uint64_t localPlayer = 0;  // FWeakObjectPtr
    std::uint64_t world       = 0;
};

struct Root  // an object followed by pointer, GUObjectArray index and class
{
    UObject*     widget = nullptr;
    std::int32_t index  = -1;
    const void*  cls    = nullptr;
};

// Installed once, then read.
platform::CodeRedirect   g_firstPlayerCode;
bool                     g_installed    = false;
std::uintptr_t           g_endUISystem  = 0;
std::uintptr_t           g_gameMode     = 0;
std::uintptr_t           g_overlay      = 0;  // the singleton's address
std::array<ObjectFn, 6>  g_subTicks{};
WeakGetFn                g_weakGet      = nullptr;
ObjectFn                 g_leaderTarget = nullptr;
CompassFn                g_compass      = nullptr;
SetPlayerContextFn       g_setContext   = nullptr;

// Read by the first-player code through its address.
alignas(8) std::uint64_t g_firstPlayerAnswer = 0;

// Game thread.
std::atomic<bool>                                       g_on{false};
bool                                                    g_refused = false;
std::vector<Root>                                       g_roots;
std::uint32_t                                           g_framesToScan = 0;
PlayerContext                                           g_contexts[2];
std::optional<player_two::primary_controller::Scope>    g_primary;
float                                                   g_savedTimeStep = 0.0f;
float*                                                  g_timeStep      = nullptr;  // saved in Enter, restored in Leave
std::array<float, 4>                                    g_playerOneRect{};
std::array<float, 4>                                    g_playerTwoRect{};
bool                                                    g_playerTwoRectKnown = false;
bool                                                    g_loggedEnter = false;
Root                                                    g_gameInstance;  // found once: FindFirstOf walks every UObject
unsigned                                                g_mask = 63;  // SetPlayerTwoLeversMask

enum Lever : unsigned
{
    kLeverContexts = 1,
    kLeverFirstPlayer = 2,
    kLeverPrimary = 4,
    kLeverRect = 8,
    kLeverUpdates = 16,
    kLeverCompass = 32,
};

bool Acts(Lever lever) noexcept
{
    return (g_mask & lever) != 0;
}
std::array<double, kParts>                              g_partSeconds{};
std::array<double, 7 + 1>                               g_tickSeconds{};  // per update, both runs (6 lists, leader icon, compass)
std::uint32_t                                           g_timedFrames = 0;
double                                                  g_secondsPerCount = 0.0;

double Now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (g_secondsPerCount == 0.0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        g_secondsPerCount = 1.0 / static_cast<double>(frequency.QuadPart);
    }
    return static_cast<double>(counter.QuadPart) * g_secondsPerCount;
}

/// Adds the time since `start` to `part`, and returns now.
double Charge(Part part, double start) noexcept
{
    const double now = Now();
    g_partSeconds[part] += now - start;
    return now;
}

int GuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(out, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(to, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool CallGuarded(ObjectFn function, void* object) noexcept
{
    __try
    {
        function(object);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool CompassGuarded(void* holder) noexcept
{
    __try
    {
        g_compass(holder, 0.0f);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool SetContextGuarded(void* widget, const PlayerContext& context) noexcept
{
    __try
    {
        g_setContext(widget, &context);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void* WeakGetGuarded(const void* weak) noexcept
{
    __try
    {
        return g_weakGet(weak);
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return nullptr;
    }
}

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD levers: {}; player 2's content stays off", why);
}

bool Alive(const Root& root) noexcept
{
    auto* item = RC::Unreal::UObjectArray::IndexToObject(root.index);
    return item != nullptr && item->GetUObject() == root.widget && !item->IsUnreachable() && !item->IsPendingKill() &&
           root.widget->GetClassPrivate() == root.cls;
}

/// A HUD root: a UUserWidget on the overlay itself (no slot), owned by the game instance.
bool IsViewportRoot(UObject* widget)
{
    auto* slot = widget->GetPropertyByNameInChain(STR("Slot"));
    if (slot == nullptr || slot->GetSize() != static_cast<std::int32_t>(sizeof(UObject*)) ||
        *widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")) != nullptr)
    {
        return false;
    }
    UObject* outer = widget->GetOuterPrivate();
    return outer != nullptr && std::wstring(outer->GetClassPrivate()->GetName()).find(L"GameInstance") != std::wstring::npos;
}

/// The game instance's local players, from the game instance found once (it lives as long as the
/// process; its liveness is checked each call).
RC::Unreal::TArray<UObject*>* CachedLocalPlayers()
{
    if (g_gameInstance.widget == nullptr || !Alive(g_gameInstance))
    {
        UObject* instance = players::unreal::GameInstance();
        g_gameInstance    = instance == nullptr ? Root{}
                                                : Root{instance, static_cast<std::int32_t>(instance->GetInternalIndex()),
                                                       instance->GetClassPrivate()};
    }
    return g_gameInstance.widget == nullptr ? nullptr : players::unreal::LocalPlayers(g_gameInstance.widget);
}

/// Refreshes the root list (new markers appear as the game needs them) and both players' contexts.
bool ScanRoots(void* playerOne, void* playerTwo)
{
    g_roots.clear();
    for (const auto& live : user_widgets::Live())  // hud/user_widgets.h: no walk of every UObject
    {
        if (IsViewportRoot(live.object))
        {
            g_roots.push_back({live.object, live.index, live.cls});
        }
    }
    // The world half of each context is the one the roots already carry (they are all player 1's,
    // RE-p2-hud.md P8.10); the player half is each local player's weak pointer.
    std::uint64_t world = 0;
    for (const Root& root : g_roots)
    {
        if (CopyGuarded(&world, reinterpret_cast<const std::uint8_t*>(root.widget) + kWidgetWorldWeak, sizeof(world)) &&
            world != 0)
        {
            break;
        }
    }
    g_contexts[0] = {player_two::primary_controller::WeakValueOf(playerOne), world};
    g_contexts[1] = {player_two::primary_controller::WeakValueOf(playerTwo), world};
    return world != 0 && g_contexts[0].localPlayer != 0 && g_contexts[1].localPlayer != 0;
}

bool SetContexts(const PlayerContext& context) noexcept
{
    for (const Root& root : g_roots)
    {
        if (Alive(root) && !SetContextGuarded(root.widget, context))
        {
            return false;
        }
    }
    return true;
}

void* EndUISystemMember(std::uint32_t member) noexcept
{
    void* object = nullptr;
    CopyGuarded(&object, reinterpret_cast<const void*>(g_endUISystem + member), sizeof(object));
    return object;
}

/// The re-runnable updates, in the game's order. False if one raised an exception.
bool RunSubTicks() noexcept
{
    for (std::size_t i = 0; Acts(kLeverUpdates) && i < kSubTicks.size(); ++i)
    {
        const double start  = Now();
        void*        object = EndUISystemMember(kSubTicks[i].member);
        if (object != nullptr && !CallGuarded(g_subTicks[i], object))
        {
            return false;
        }
        g_tickSeconds[i] += Now() - start;
    }
    const double leaderStart = Now();
    auto* leader = static_cast<std::uint8_t*>(EndUISystemMember(kLeaderTargetMember));
    std::int32_t count = 0;
    if (Acts(kLeverUpdates) && leader != nullptr && CopyGuarded(&count, leader + kLeaderTargetCount, sizeof(count)) &&
        count != 0)
    {
        if (void* target = WeakGetGuarded(leader + kLeaderTargetWeak); target != nullptr && !CallGuarded(g_leaderTarget, target))
        {
            return false;
        }
    }
    g_tickSeconds[kSubTicks.size()] += Now() - leaderStart;
    const double compassStart = Now();
    auto*       manager = static_cast<std::uint8_t*>(EndUISystemMember(kCompassManager));
    void*       holder  = nullptr;
    if (Acts(kLeverCompass) && manager != nullptr && CopyGuarded(&holder, manager + kCompassHolder, sizeof(holder)) &&
        holder != nullptr &&
        !CompassGuarded(holder))
    {
        return false;
    }
    g_tickSeconds[kSubTicks.size() + 1] += Now() - compassStart;
    return true;
}

void LogTiming()
{
    if (++g_timedFrames < kTimingFrames)
    {
        return;
    }
    const double perFrame = 1000.0 / g_timedFrames;
    std::wstring parts;
    for (std::size_t i = 0; i < kParts; ++i)
    {
        parts += std::format(L"{}{} {:.3f}", i == 0 ? L"" : L", ", kPartNames[i], g_partSeconds[i] * perFrame);
    }
    std::wstring ticks;
    constexpr const wchar_t* kTickNames[] = {L"+98", L"+B0", L"+150", L"+188", L"+170", L"+180", L"leader", L"compass"};
    for (std::size_t i = 0; i < g_tickSeconds.size(); ++i)
    {
        ticks += std::format(L" {} {:.3f}", kTickNames[i], g_tickSeconds[i] * perFrame);
    }
    platform::Log(L"HUD levers: ms per frame over {} frames: {}; updates (both runs):{}; {} user widgets live", g_timedFrames,
                  parts, ticks, user_widgets::Live().size());
    g_partSeconds.fill(0.0);
    g_tickSeconds.fill(0.0);
    g_timedFrames = 0;
}

float* TimeStepField() noexcept
{
    std::uintptr_t mode = 0;
    if (!CopyGuarded(&mode, reinterpret_cast<const void*>(g_gameMode), sizeof(mode)) || mode == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<float*>(mode + kGameModeHudTimeStep);
}

std::uintptr_t Overlay() noexcept
{
    std::uintptr_t overlay = 0;
    CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlay), sizeof(overlay));
    return overlay;
}

bool ReadRect(std::array<float, 4>& rect) noexcept
{
    const std::uintptr_t overlay = Overlay();
    for (std::size_t i = 0; overlay != 0 && i < rect.size(); ++i)
    {
        if (!CopyGuarded(&rect[i], reinterpret_cast<const void*>(overlay + kOverlayRectFields[i]), sizeof(float)))
        {
            return false;
        }
    }
    return overlay != 0;
}

void WriteRect(const std::array<float, 4>& rect) noexcept
{
    const std::uintptr_t overlay = Overlay();
    for (std::size_t i = 0; overlay != 0 && i < rect.size(); ++i)
    {
        WriteGuarded(reinterpret_cast<void*>(overlay + kOverlayRectFields[i]), &rect[i], sizeof(float));
    }
}

}  // namespace

bool InstallPlayerTwoPass(const platform::GameImage& image) noexcept
{
    bool holds = image.BytesMatch(kWeakGet, kWeakGetPrologue) &&
                 image.BytesMatch(kLeaderTargetTick, kLeaderTargetPrologue) &&
                 image.BytesMatch(kSetPlayerContext, kSetPlayerContextPrologue) &&
                 image.BytesMatch(kCompassFrameUpdate, kCompassFrameUpdatePrologue);
    for (std::size_t i = 0; holds && i < kSubTicks.size(); ++i)
    {
        const auto live = image.Rebase(kSubTicks[i].function);
        holds           = live && image.BytesMatch(kSubTicks[i].function, kSubTicks[i].prologue);
        g_subTicks[i]   = live ? reinterpret_cast<ObjectFn>(*live) : nullptr;
    }
    const auto system  = image.Rebase(kEndUISystem, kSubTicks.back().member + sizeof(void*));
    const auto mode    = image.Rebase(kGameMode, sizeof(void*));
    const auto overlay = image.Rebase(engine::kRootOverlaySingleton, sizeof(void*));
    const auto get     = image.Rebase(kWeakGet);
    const auto leader  = image.Rebase(kLeaderTargetTick);
    const auto context = image.Rebase(kSetPlayerContext);
    const auto compass = image.Rebase(kCompassFrameUpdate);
    const auto entry   = image.Rebase(kFirstGamePlayer, sizeof(kFirstPlayerEntryBytes));
    const auto resume  = image.Rebase(kFirstGamePlayerResume);
    if (!holds || !system || !mode || !overlay || !get || !leader || !context || !compass || !entry || !resume)
    {
        platform::Log(L"HUD levers: the HUD's update code is not what the research read; player 2's content unavailable");
        return false;
    }
    g_endUISystem  = *system;
    g_gameMode     = *mode;
    g_overlay      = *overlay;
    g_weakGet      = reinterpret_cast<WeakGetFn>(*get);
    g_leaderTarget = reinterpret_cast<ObjectFn>(*leader);
    g_setContext   = reinterpret_cast<SetPlayerContextFn>(*context);
    g_compass      = reinterpret_cast<CompassFn>(*compass);

    // The first-player code reads g_firstPlayerAnswer, in this DLL: pin it before installing.
    const auto code = FirstPlayerCode(g_firstPlayerAnswer);
    if (!platform::PinThisModule() || !g_firstPlayerCode.Install(*entry, kFirstPlayerEntryBytes, code, *resume))
    {
        platform::Log(L"HUD levers: the first-player function's entry could not be redirected; player 2's content "
                      L"unavailable");
        return false;
    }
    g_installed = true;
    return true;
}

void RemovePlayerTwoPass() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_firstPlayerAnswer = 0;
    if (g_firstPlayerCode.IsInstalled() && !g_firstPlayerCode.Restore())
    {
        platform::Log(L"HUD levers: the first-player entry was changed after us; left in place (it answers as the game "
                      L"does while the levers are off)");
    }
    g_installed = false;
}

bool PlayerTwoPassInstalled() noexcept
{
    return g_installed;
}

void SetPlayerTwoContent(bool on) noexcept
{
    g_on.store(on && g_installed && !g_refused, std::memory_order_relaxed);
    g_framesToScan       = 0;
    g_playerTwoRectKnown = false;
    g_loggedEnter        = false;
    platform::Log(L"HUD levers: player 2's content {}", PlayerTwoContentOn() ? L"on" : on ? L"unavailable" : L"off");
}

void SetPlayerTwoLeversMask(unsigned mask) noexcept
{
    g_mask = mask;
    platform::Log(L"HUD levers: mask {} (contexts {}, first player {}, primary {}, rect {}, updates {}, compass {})", mask,
                  Acts(kLeverContexts), Acts(kLeverFirstPlayer), Acts(kLeverPrimary), Acts(kLeverRect), Acts(kLeverUpdates),
                  Acts(kLeverCompass));
}

bool PlayerTwoContentOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

bool EnterPlayerTwo() noexcept
{
    try
    {
        if (!PlayerTwoContentOn())
        {
            return false;
        }
        auto* list = CachedLocalPlayers();
        if (list == nullptr || list->Num() < 2)
        {
            return false;
        }
        UObject* playerOne = (*list)[0];
        UObject* playerTwo = (*list)[1];
        double mark = Now();
        if (g_framesToScan == 0)
        {
            const bool scanned = ScanRoots(playerOne, playerTwo);
            mark               = Charge(kScan, mark);
            if (!scanned)
            {
                return false;  // g_framesToScan stays 0: retried next frame
            }
            g_framesToScan = kRootScanFrames;
        }
        --g_framesToScan;

        if (Acts(kLeverPrimary))
        {
            g_primary.emplace();
        }
        if (g_primary && !g_primary->Held())
        {
            platform::Log(L"HUD levers: the primary controller could not be switched ({}); this frame keeps player 1's content",
                          player_two::primary_controller::Describe(g_primary->Why()));
            g_primary.reset();
            return false;
        }
        g_firstPlayerAnswer = Acts(kLeverFirstPlayer) ? reinterpret_cast<std::uint64_t>(playerTwo) : 0;  // L2
        mark = Now();
        const bool contexts = !Acts(kLeverContexts) || SetContexts(g_contexts[1]);  // L1
        mark                = Charge(kContextsTwo, mark);
        if (!contexts)
        {
            g_firstPlayerAnswer = 0;
            const bool reverted = SetContexts(g_contexts[0]);
            g_primary.reset();
            Refuse(reverted ? L"SetPlayerContext raised an exception"
                            : L"SetPlayerContext raised an exception, and again putting player 1's back (some roots may "
                              L"keep player 2's context until the next join)");
            return false;
        }
        if (float* step = TimeStepField())                                  // L6
        {
            const float still = 0.0f;
            if (CopyGuarded(&g_savedTimeStep, step, sizeof(float)))
            {
                g_timeStep = step;
                WriteGuarded(step, &still, sizeof(float));
            }
        }
        ReadRect(g_playerOneRect);
        if (g_playerTwoRectKnown && Acts(kLeverRect))
        {
            WriteRect(g_playerTwoRect);  // what player 2's render computed last frame (via L2)
        }
        mark            = Now();
        const bool runs = RunSubTicks();
        Charge(kTicksTwo, mark);
        if (!runs)
        {
            Refuse(L"a HUD update raised an exception in player 2's run");
        }
        if (!g_loggedEnter)
        {
            g_loggedEnter = true;
            platform::Log(L"HUD levers: player 2's pass with {} HUD root(s); player 2's view rect {}", g_roots.size(),
                          g_playerTwoRectKnown ? L"from the last frame" : L"not known yet (first frame)");
        }
        return true;
    }
    catch (...)
    {
        // Only the log lines allocate; whatever was switched before the throw is put back.
        LeavePlayerTwo();
        g_on.store(false, std::memory_order_relaxed);
        return false;
    }
}

void LeavePlayerTwo() noexcept
{
    // Every lever is put back whatever happens to the others: each step is guarded on its own
    // (the engine calls by SEH inside the helpers), and nothing here allocates except the log.
    g_playerTwoRectKnown = ReadRect(g_playerTwoRect);  // computed by player 2's render through L2
    g_firstPlayerAnswer  = 0;
    double     mark      = Now();
    const bool contexts  = !Acts(kLeverContexts) || SetContexts(g_contexts[0]);
    mark                 = Charge(kContextsOne, mark);
    g_primary.reset();  // SwapWindow's close: a no-op unless it opened
    WriteRect(g_playerOneRect);
    mark             = Now();
    const bool ticks = RunSubTicks();  // player 1's positions again, before player 1's render
    Charge(kTicksOne, mark);
    if (g_timeStep != nullptr)
    {
        WriteGuarded(g_timeStep, &g_savedTimeStep, sizeof(float));
        g_timeStep = nullptr;
    }
    try
    {
        LogTiming();
        if (!contexts || !ticks)
        {
            Refuse(!contexts ? L"SetPlayerContext raised an exception restoring player 1 (some roots may keep player 2's "
                               L"context until the next join)"
                             : L"a HUD update raised an exception in player 1's run");
        }
    }
    catch (...)
    {
        g_on.store(false, std::memory_order_relaxed);  // Refuse without its log line
    }
}

}  // namespace lss::hud
