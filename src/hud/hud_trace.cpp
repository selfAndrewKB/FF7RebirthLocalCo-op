#include "hud/hud_trace.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "players/unreal_access.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

using RC::Unreal::UObject;

// Call sites and targets, as IDA shows them (RE-p2-hud.md P8.9; bytes read from the database).
// Each target's prologue up to its stack probe call is checked before anything is redirected.
struct Site
{
    std::uintptr_t call;    // an E8 rel32 inside one aligned 16-byte block
    std::uintptr_t target;
};

constexpr std::uintptr_t kMasterHudTick = 0x140F37E08;  // void (EndUISystem*, float DeltaTime)
constexpr std::uint8_t   kMasterHudTickPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr Site           kMasterHudTickSite{0x1423EF4A1, kMasterHudTick};  // AEndGameMode__Tick530_Body


// bool (APlayerController*, UUserWidget* marker, const FVector*, float* inout)
constexpr std::uintptr_t kPlaceMarker = 0x140A59A30;
constexpr std::uint8_t   kPlaceMarkerPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
                                                   0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr Site           kPlaceMarkerSite{0x140C8D620, kPlaceMarker};  // EndUI__MarkerList_Tick

// bool (APlayerController*, const FVector*, FVector2D* out)
constexpr std::uintptr_t kProject = 0x140CBF974;
constexpr std::uint8_t   kProjectPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                               0x24, 0x10, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr std::array<Site, 4> kProjectSites{{
    {0x1409F94FA, kProject},  // AreaMapIcon_FrameUpdate
    {0x1409FA4F6, kProject},  // AreaMapIcon_FrameUpdate
    {0x1408607C5, kProject},  // AreaMapIcon__ProjectIconElement
    {0x140860829, kProject},  // AreaMapIcon__ProjectIconElement
}};

constexpr std::uintptr_t kFrameNumber = 0x146FAA370;  // GFrameNumber, uint32

// EndUISystem (g_EndUISystem) and the members P8.9 read: 8-byte heap pointers, weak object
// pointers as {int32 index, int32 serial} (serial 0 = null), and one-byte flags.
constexpr std::uintptr_t kEndUISystem               = 0x1493496E0;
constexpr std::uint32_t  kFieldCommandCtrl          = 0xB8;
constexpr std::uint32_t  kCompassManager            = 0x108;
constexpr std::uint32_t  kCompassHolder             = 0x348;   // in the compass manager; its +0 is the weak widget
// EndUISystem byte. 1.005: MasterHUD_Tick 0x140F75A05 cmp byte [rbx+2D9h],0 (1.004 0x140F37E5D [rbx+2D1h]);
// EndUISystem__SetupFieldCommandOrDefer 0x1413FBB9C uses +0x2D9/+0x2DC (a member was inserted after +0x2A8).
constexpr platform::PerBuild<std::uint32_t> kFieldCommandSetupPending = {0x2D1, 0x2D9};
constexpr std::uint32_t  kCtrlCommandMenuWidget     = 0x4F0;   // FieldCommandCtrl weak widgets and controller
constexpr std::uint32_t  kCtrlFieldMenuWidget       = 0x4E8;
constexpr std::uint32_t  kCtrlStoredController      = 0x558;
constexpr std::uint32_t  kCtrlSetupTrigger          = 0x775;   // FieldCommandCtrl byte

// SGameLayerManager::GetPlayerWidgetHostGeometry: IGameLayerManager vtable slot 2,
// FGeometry* (IGameLayerManager*, FGeometry* out, ULocalPlayer*). The interface pointer is held
// by the game viewport client at +0x120 (a weak pointer; its reference controller at +0x128).
constexpr std::uintptr_t kPlayerHostGeometry = 0x1451803CC;
constexpr std::uint8_t   kPlayerHostGeometryPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                          0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint32_t  kViewportLayerManager           = 0x120;
constexpr std::uint32_t  kViewportLayerManagerController = 0x128;
constexpr std::size_t    kGeometryBytes                  = 0x38;  // Size +0, Scale +8, translation +0x2C


using HudTickFn  = void (*)(void* system, float deltaTime);
using MarkerFn   = bool (*)(void* controller, void* marker, const void* location, float* inout);
using ProjectFn  = bool (*)(void* controller, const void* location, void* out);
using GeometryFn = void* (*)(void* layerManager, void* out, void* localPlayer);

platform::CallRedirect                 g_tickCall;
platform::CallRedirect                 g_markerCall;
std::array<platform::CallRedirect, 4>  g_projectCalls;
// Game version 1.005 (RE-1005-hud-2.md): two project sites (0x1409B810C, 0x140808AEE there) cross a 16-byte block. The
// game thread alone runs them, so they are installed from there, around the overlay render (TraceBeforeRender), and
// only restored from there.
std::array<std::uintptr_t, 4>          g_projectPending{};  // live call sites still to install (0: none)
std::array<bool, 4>                    g_projectCrosses{};
std::uintptr_t                         g_project          = 0;  // the live target, for every project redirect
bool                                   g_installed        = false;
std::uintptr_t                         g_frameNumber      = 0;
std::uintptr_t                         g_endUISystem      = 0;
std::uintptr_t                         g_playerHostGeometry = 0;
std::atomic<std::int32_t>              g_framesLeft{0};

/// What one frame did, game thread only.
struct Frame
{
    std::wstring  order;               // T/M/P/R, runs compressed as they are appended
    wchar_t       last  = 0;
    std::uint32_t run   = 0;
    std::uint32_t ticks = 0;
    float         deltaTime = 0.0f;
    std::array<std::uint32_t, 4> markers{};  // by player: 1, 2, other, none
    std::array<std::uint32_t, 4> projections{};
};
Frame g_frame;

std::uint32_t FrameNumber() noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(g_frameNumber), sizeof(value));
    return value;
}

std::size_t PlayerSlot(const void* controller) noexcept
{
    if (controller == nullptr)
    {
        return 3;
    }
    if (controller == player_two::generation::PlayerOneController())
    {
        return 0;
    }
    return controller == player_two::generation::PlayerTwoController() ? 1 : 2;
}

void FlushRun()
{
    if (g_frame.run > 0)
    {
        g_frame.order += g_frame.run > 1 ? std::format(L"{}x{} ", g_frame.last, g_frame.run) : std::format(L"{} ", g_frame.last);
    }
}

// The replacements below are entered from the game's code through a bare jump, so nothing may
// leave them by exception: the logging swallows its own failures (an allocation, a format).

void Note(wchar_t event) noexcept
{
    try
    {
        if (event != g_frame.last)
        {
            FlushRun();
            g_frame.last = event;
            g_frame.run  = 0;
        }
        ++g_frame.run;
    }
    catch (...)
    {
    }
}

std::wstring ByPlayer(const std::array<std::uint32_t, 4>& counts)
{
    return std::format(L"player 1 {}, player 2 {}, other {}, none {}", counts[0], counts[1], counts[2], counts[3]);
}

bool Tracing() noexcept
{
    return g_framesLeft.load(std::memory_order_relaxed) > 0 && player_two::generation::OnGameThread();
}

void LogFrame() noexcept
{
    try
    {
        FlushRun();
        platform::Log(L"hud trace: frame {}: {}| HUD ticks {} (DeltaTime {:.4f}); markers placed with {}; icons projected with {}",
                      FrameNumber(), g_frame.order, g_frame.ticks, g_frame.deltaTime, ByPlayer(g_frame.markers),
                      ByPlayer(g_frame.projections));
    }
    catch (...)
    {
    }
    g_frame = {};
    if (g_framesLeft.fetch_sub(1, std::memory_order_relaxed) == 1)
    {
        platform::Log(L"hud trace: done");
    }
}

void TracedHudTick(void* system, float deltaTime) noexcept
{
    if (Tracing())
    {
        Note(L'T');
        ++g_frame.ticks;
        g_frame.deltaTime = deltaTime;
    }
    reinterpret_cast<HudTickFn>(g_tickCall.Original())(system, deltaTime);
}

bool TracedMarker(void* controller, void* marker, const void* location, float* inout) noexcept
{
    if (Tracing())
    {
        Note(L'M');
        ++g_frame.markers[PlayerSlot(controller)];
    }
    return reinterpret_cast<MarkerFn>(g_markerCall.Original())(controller, marker, location, inout);
}

bool TracedProject(void* controller, const void* location, void* out) noexcept
{
    if (Tracing())
    {
        Note(L'P');
        ++g_frame.projections[PlayerSlot(controller)];
    }
    // Every project site calls the same function (kProjectSites), set before any of them is redirected.
    return reinterpret_cast<ProjectFn>(g_project)(controller, location, out);
}



bool Redirect(const platform::GameImage& image, platform::CallRedirect& call, const Site& site,
              std::uintptr_t replacement) noexcept
{
    const auto live   = image.Rebase(site.call, 5);
    const auto target = image.Rebase(site.target);
    return live && target && call.Install(*live, *target, replacement);
}

bool CopyGuarded(void* out, std::uintptr_t from, std::size_t length) noexcept
{
    return platform::CopyGuarded(out, reinterpret_cast<const void*>(from), length);
}

std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address) noexcept
{
    std::uintptr_t value = 0;
    return address != 0 && CopyGuarded(&value, address, sizeof(value)) ? std::optional(value) : std::nullopt;
}

std::optional<std::uint8_t> ReadByte(std::uintptr_t address) noexcept
{
    std::uint8_t value = 0;
    return address != 0 && CopyGuarded(&value, address, sizeof(value)) ? std::optional(value) : std::nullopt;
}

bool CallGeometry(GeometryFn function, void* layerManager, void* out, void* localPlayer) noexcept
{
    __try
    {
        function(layerManager, out, localPlayer);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// A weak object pointer at `address`, resolved through GUObjectArray with its serial checked.
std::wstring DescribeWeak(std::uintptr_t address)
{
    std::int32_t weak[2] = {};
    if (address == 0 || !CopyGuarded(weak, address, sizeof(weak)))
    {
        return L"(unreadable)";
    }
    if (weak[1] == 0)
    {
        return L"(null)";
    }
    auto* item = RC::Unreal::UObjectArray::IndexToObject(weak[0]);
    if (item == nullptr || item->GetUObject() == nullptr || item->GetSerialNumber() != weak[1])
    {
        return std::format(L"(stale: index {}, serial {})", weak[0], weak[1]);
    }
    UObject* object = item->GetUObject();
    const auto slot = PlayerSlot(object);
    const std::wstring who = slot == 0 ? L" = player 1's controller" : slot == 1 ? L" = player 2's controller" : L"";
    return std::format(L"{} '{}'{}", std::wstring(object->GetClassPrivate()->GetName()), std::wstring(object->GetName()), who);
}

std::wstring Flag(std::uintptr_t address)
{
    const auto value = ReadByte(address);
    return value ? std::to_wstring(*value) : std::wstring(L"(unreadable)");
}

int LogPlayerLayers()
{
    UObject* engine   = RC::Unreal::UObjectGlobals::FindFirstOf(STR("GameEngine"));
    auto*    viewport = engine != nullptr && engine->GetPropertyByNameInChain(STR("GameViewport")) != nullptr
                            ? *engine->GetValuePtrByPropertyNameInChain<UObject*>(STR("GameViewport"))
                            : nullptr;
    const auto base     = reinterpret_cast<std::uintptr_t>(viewport);
    const auto manager  = viewport != nullptr ? ReadPointer(base + kViewportLayerManager) : std::nullopt;
    const auto refCount = viewport != nullptr ? ReadPointer(base + kViewportLayerManagerController) : std::nullopt;
    if (g_playerHostGeometry == 0 || !manager || *manager == 0 || !refCount || *refCount == 0)
    {
        platform::Log(L"hud managers: the game layer manager is not reachable; player layers not read");
        return 1;
    }
    const auto vtable = ReadPointer(*manager);
    const auto slot2  = vtable ? ReadPointer(*vtable + 2 * sizeof(void*)) : std::nullopt;
    if (!slot2 || *slot2 != g_playerHostGeometry)
    {
        platform::Log(L"hud managers: the layer manager's vtable slot 2 is not GetPlayerWidgetHostGeometry; not called");
        return 1;
    }
    int lines = 0;
    auto* list = players::unreal::LocalPlayers(players::unreal::GameInstance());
    for (std::int32_t i = 0; list != nullptr && i < list->Num(); ++i)
    {
        alignas(16) std::uint8_t geometry[kGeometryBytes]{};
        if (!CallGeometry(reinterpret_cast<GeometryFn>(*slot2), reinterpret_cast<void*>(*manager), geometry, (*list)[i]))
        {
            platform::Log(L"hud managers: GetPlayerWidgetHostGeometry raised an exception for LocalPlayers[{}]", i);
            ++lines;
            continue;
        }
        float size[2] = {}, scale = 0.0f, translation[2] = {};
        std::memcpy(size, geometry, sizeof(size));
        std::memcpy(&scale, geometry + 8, sizeof(scale));
        std::memcpy(translation, geometry + 0x2C, sizeof(translation));
        platform::Log(L"hud managers: LocalPlayers[{}]'s stock player layer: size {:.0f}x{:.0f}, scale {:.3f}, at ({:.0f}, {:.0f})",
                      i, size[0], size[1], scale, translation[0], translation[1]);
        ++lines;
    }
    return lines;
}

}  // namespace

bool InstallHudTrace(const platform::GameImage& image) noexcept
{
    const bool prologues = image.BytesMatch(kMasterHudTick, kMasterHudTickPrologue) &&

                           image.BytesMatch(kPlaceMarker, kPlaceMarkerPrologue) &&
                           image.BytesMatch(kProject, kProjectPrologue);
    const auto frame  = image.Rebase(kFrameNumber, sizeof(std::uint32_t));
    const auto system = image.Rebase(kEndUISystem, kFieldCommandSetupPending + 1);
    if (!prologues || !frame || !system)
    {
        platform::Log(L"HUD trace: the functions are not what the research read; no trace");
        return false;
    }
    g_frameNumber = *frame;
    g_endUISystem = *system;
    if (image.BytesMatch(kPlayerHostGeometry, kPlayerHostGeometryPrologue))
    {
        g_playerHostGeometry = image.Rebase(kPlayerHostGeometry).value_or(0);
    }

    g_project      = image.Rebase(kProject).value_or(0);
    bool installed = g_project != 0 &&
                     Redirect(image, g_tickCall, kMasterHudTickSite, reinterpret_cast<std::uintptr_t>(&TracedHudTick)) &&

                     Redirect(image, g_markerCall, kPlaceMarkerSite, reinterpret_cast<std::uintptr_t>(&TracedMarker));
    const bool v1005 = platform::RunningBuild() == platform::BuildIndex::V1005;
    for (std::size_t i = 0; installed && i < kProjectSites.size(); ++i)
    {
        const auto site = image.Rebase(kProjectSites[i].call, 5);
        if (v1005 && site && (*site & 0xF) + 5 > 16)
        {
            g_projectPending[i] = *site;  // installed from the game thread (above)
            g_projectCrosses[i] = true;
            continue;
        }
        installed = Redirect(image, g_projectCalls[i], kProjectSites[i], reinterpret_cast<std::uintptr_t>(&TracedProject));
    }
    if (!installed)
    {
        RemoveHudTrace();
        platform::Log(L"HUD trace: a call site is not what the research read; no trace");
        return false;
    }
    g_installed = true;
    if (v1005)
    {
        platform::Log(L"HUD trace: installed (the project calls crossing a block follow on the game thread)");
    }
    return true;
}

void RemoveHudTrace() noexcept
{
    g_framesLeft.store(0, std::memory_order_relaxed);
    g_projectPending = {};
    bool all      = true;
    auto restore  = [&all](platform::CallRedirect& call) {
        if (call.IsInstalled() && !call.Restore())
        {
            all = false;
        }
    };
    restore(g_tickCall);
    restore(g_markerCall);
    const bool gameThread = player_two::generation::OnGameThread();
    for (std::size_t i = 0; i < g_projectCalls.size(); ++i)
    {
        if (!g_projectCrosses[i] || gameThread)  // a crossing call: only from the game thread (else it passes through)
        {
            restore(g_projectCalls[i]);
        }
    }
    if (!all)
    {
        platform::Log(L"HUD trace: a redirect was changed after us; left in place, passing the calls through");
    }
    g_installed = false;
}

bool HudTraceInstalled() noexcept
{
    return g_installed;
}

void TraceBeforeRender() noexcept
{
    if (g_installed && g_project != 0 && player_two::generation::OnGameThread())
    {
        // Here the game thread is outside AreaMapIcon's frame update, as InstallFromOwnThread requires.
        for (std::size_t i = 0; i < g_projectPending.size(); ++i)
        {
            if (g_projectPending[i] == 0)
            {
                continue;
            }
            const bool ok = g_projectCalls[i].InstallFromOwnThread(g_projectPending[i], g_project,
                                                                   reinterpret_cast<std::uintptr_t>(&TracedProject));
            g_projectPending[i] = 0;
            platform::Log(L"HUD trace: project call {} {}", i + 1,
                          ok ? L"installed from the game thread" : L"could not be redirected (its icons are not counted)");
        }
    }
    if (Tracing())
    {
        Note(L'R');
    }
}

void TraceAfterRender() noexcept
{
    // A frame is logged at its render; the trace is only started once the render hook is in.
    if (Tracing() && g_frame.run > 0)
    {
        LogFrame();
    }
}

void StartHudTrace(std::int32_t frames) noexcept
{
    g_frame = {};
    g_framesLeft.store(frames < 1 ? 1 : frames > kMaxHudTraceFrames ? kMaxHudTraceFrames : frames, std::memory_order_relaxed);
    platform::Log(L"hud trace: logging {} frame(s)", g_framesLeft.load(std::memory_order_relaxed));
}

int LogHudManagers() noexcept
{
    try
    {
        if (g_endUISystem == 0)
        {
            platform::Log(L"hud managers: EndUISystem not located (the trace did not install)");
            return 1;
        }
        const auto ctrl    = ReadPointer(g_endUISystem + kFieldCommandCtrl).value_or(0);
        const auto compass = ReadPointer(g_endUISystem + kCompassManager).value_or(0);
        const auto holder  = compass != 0 ? ReadPointer(compass + kCompassHolder).value_or(0) : 0;
        platform::Log(L"hud managers: EndUISystem field-command setup pending {}; field command controller {}",
                      Flag(g_endUISystem + kFieldCommandSetupPending), ctrl != 0 ? L"present" : L"absent");
        int lines = 1;
        if (ctrl != 0)
        {
            platform::Log(L"hud managers: field command: command menu {}; field menu {}; stored controller {}; setup trigger {}",
                          DescribeWeak(ctrl + kCtrlCommandMenuWidget), DescribeWeak(ctrl + kCtrlFieldMenuWidget),
                          DescribeWeak(ctrl + kCtrlStoredController), Flag(ctrl + kCtrlSetupTrigger));
            ++lines;
        }
        platform::Log(L"hud managers: compass holder's widget {}",
                      holder != 0 ? DescribeWeak(holder) : std::wstring(L"(no holder)"));
        ++lines;
        return lines + LogPlayerLayers();
    }
    catch (...)
    {
        platform::Log(L"hud managers: failed (an exception while reading)");
        return 1;
    }
}

}  // namespace lss::hud
