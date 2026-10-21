#include "hud/hud_trace.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>

namespace lss::hud {

namespace {

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

using HudTickFn  = void (*)(void* system, float deltaTime);
using MarkerFn   = bool (*)(void* controller, void* marker, const void* location, float* inout);
using ProjectFn  = bool (*)(void* controller, const void* location, void* out);

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



}  // namespace

bool InstallHudTrace(const platform::GameImage& image) noexcept
{
    const bool prologues = image.BytesMatch(kMasterHudTick, kMasterHudTickPrologue) &&

                           image.BytesMatch(kPlaceMarker, kPlaceMarkerPrologue) &&
                           image.BytesMatch(kProject, kProjectPrologue);
    const auto frame  = image.Rebase(kFrameNumber, sizeof(std::uint32_t));
    if (!prologues || !frame)
    {
        platform::Log(L"HUD trace: the functions are not what the research read; no trace");
        return false;
    }
    g_frameNumber = *frame;
    g_project      = image.Rebase(kProject).value_or(0);
    bool installed = g_project != 0 &&
                     g_tickCall.InstallResearch(image, kMasterHudTickSite.call, kMasterHudTickSite.target,
                                                reinterpret_cast<std::uintptr_t>(&TracedHudTick)) &&

                     g_markerCall.InstallResearch(image, kPlaceMarkerSite.call, kPlaceMarkerSite.target,
                                                  reinterpret_cast<std::uintptr_t>(&TracedMarker));
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
        installed = g_projectCalls[i].InstallResearch(image, kProjectSites[i].call, kProjectSites[i].target,
                                                      reinterpret_cast<std::uintptr_t>(&TracedProject));
    }
    if (!installed)
    {
        RemoveHudTrace();
        platform::Log(L"HUD trace: a call site is not what the research read; no trace");
        return false;
    }
    g_installed = true;
    platform::Log(L"HUD trace: installed{}",
                  v1005 ? L" (the project calls crossing a block follow at the next overlay render)" : L"");
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

}  // namespace lss::hud
