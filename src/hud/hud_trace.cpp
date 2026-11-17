#include "hud/hud_trace.h"

#include "facts/engine.h"

#include "hud/passes/root_overlay_render.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>

namespace lss::hud {

namespace {

// Call sites and targets, as IDA shows them (research/RE-p2-hud.md; bytes read from the database). Each
// target's prologue up to its stack probe call is checked before anything is redirected; each call
// is an E8 rel32 inside one aligned 16-byte block.

constexpr std::uint8_t   kMasterHudTickPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
// void (EndUISystem*, float DeltaTime)
constexpr platform::FunctionFact kMasterHudTick{L"EndUISystem MasterHUD_Tick", 0x140F759B0, kMasterHudTickPrologue};
constexpr std::uintptr_t kMasterHudTickCall = 0x140B38A66;  // in AEndGameMode__Tick530_Body


// bool (APlayerController*, UUserWidget* marker, const FVector*, float* inout)
constexpr std::uint8_t   kPlaceMarkerPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
                                                   0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::FunctionFact kPlaceMarker{L"EndUI__PlaceMarkerWidgetAtWorld_Clamped", 0x140E8EA08,
                                              kPlaceMarkerPrologue};
constexpr std::uintptr_t kPlaceMarkerCall = 0x140F75348;  // in EndUI__MarkerList_Tick

// bool (APlayerController*, const FVector*, FVector2D* out)
constexpr std::uint8_t   kProjectPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                               0x24, 0x10, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kProject{L"the HUD's project-to-screen (controller, point, out)", 0x140BC8AB8,
                                          kProjectPrologue};
constexpr std::array<std::uintptr_t, 4> kProjectCalls{{
    0x1409B810C,  // AreaMapIcon_FrameUpdate
    0x1409B9134,  // AreaMapIcon_FrameUpdate
    0x140808949,  // AreaMapIcon__ProjectIconElement
    0x140808AEE,  // AreaMapIcon__ProjectIconElement
}};

// GFrameNumber: facts::kFrameNumber (facts/engine.h; per build, the 1.005 twin in the hand-found table).

using HudTickFn  = void (*)(void* system, float deltaTime);
using MarkerFn   = bool (*)(void* controller, void* marker, const void* location, float* inout);
using ProjectFn  = bool (*)(void* controller, const void* location, void* out);

platform::CallRedirect                 g_tickCall;
platform::CallRedirect                 g_markerCall;
std::array<platform::CallRedirect, 4>  g_projectCalls;
// Game version 1.005 (research/RE-1005-hud-2.md): two project sites cross a 16-byte block. The
// game thread alone runs them, so they are installed from there, around the overlay render (TraceBeforeRender), and
// only restored from there.
std::array<std::uintptr_t, 4>          g_projectPending{};  // live call sites still to install (0: none)
std::array<bool, 4>                    g_projectCrosses{};
std::uintptr_t                         g_project          = 0;  // the live target, for every project redirect
bool                                   g_installed        = false;
std::uintptr_t                         g_frameNumber      = 0;
std::atomic<std::int32_t>              g_framesLeft{0};

/// Counts by whose controller a call used: each player's (0 player 1's, then the seats'), another's, none.
constexpr std::size_t kOtherSlot = players::kMaxPlayers;
constexpr std::size_t kNoneSlot  = kOtherSlot + 1;
using ByPlayerCounts = std::array<std::uint32_t, kNoneSlot + 1>;

/// What one frame did, game thread only.
struct Frame
{
    std::wstring  order;               // T/M/P/R, runs compressed as they are appended
    wchar_t       last  = 0;
    std::uint32_t run   = 0;
    std::uint32_t ticks = 0;
    float         deltaTime = 0.0f;
    ByPlayerCounts markers{};
    ByPlayerCounts projections{};
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
        return kNoneSlot;
    }
    if (controller == players::generation::PlayerOneController())
    {
        return 0;
    }
    const int seat = players::generation::SeatOfController(controller);
    return players::SeatIndex(seat) ? static_cast<std::size_t>(seat) : kOtherSlot;
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

std::wstring ByPlayer(const ByPlayerCounts& counts)
{
    std::wstring text;
    for (int p = 0; p < players::kMaxPlayers; ++p)
    {
        text += std::format(L"{} {}, ", players::SeatName(p), counts[static_cast<std::size_t>(p)]);
    }
    return text + std::format(L"other {}, none {}", counts[kOtherSlot], counts[kNoneSlot]);
}

bool Tracing() noexcept
{
    return g_framesLeft.load(std::memory_order_relaxed) > 0 && players::generation::OnGameThread();
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
    // Every project site calls the same function (kProjectCalls), set before any of them is redirected.
    return reinterpret_cast<ProjectFn>(g_project)(controller, location, out);
}



}  // namespace

bool InstallHudTrace(const platform::GameImage& image) noexcept
{
    const auto tick    = platform::FindFunction(image, kMasterHudTick);
    const auto marker  = platform::FindFunction(image, kPlaceMarker);
    const auto project = platform::FindFunction(image, kProject);
    const auto frame   = players::game_globals::Address(players::game_globals::Global::FrameNumber);
    if (!tick || !marker || !project || !frame)
    {
        platform::Log(L"HUD trace: the functions are not what the research read; no trace");
        return false;
    }
    g_frameNumber  = frame;
    g_project      = *project;
    bool installed = g_tickCall.Install(image, kMasterHudTickCall, *tick,
                                        reinterpret_cast<std::uintptr_t>(&TracedHudTick)) &&
                     g_markerCall.Install(image, kPlaceMarkerCall, *marker,
                                          reinterpret_cast<std::uintptr_t>(&TracedMarker));
    const bool crossing = platform::RunningBuild() != platform::BuildIndex::V1004;  // two project calls cross a block
    for (std::size_t i = 0; installed && i < kProjectCalls.size(); ++i)
    {
        const auto site = image.Rebase(kProjectCalls[i], 5);
        if (crossing && site && (*site & 0xF) + 5 > 16)
        {
            g_projectPending[i] = *site;  // installed from the game thread (above)
            g_projectCrosses[i] = true;
            continue;
        }
        installed = g_projectCalls[i].Install(image, kProjectCalls[i], *project,
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
                  crossing ? L" (the project calls crossing a block follow at the next overlay render)" : L"");
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
    const bool gameThread = players::generation::OnGameThread();
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
    if (g_installed && g_project != 0 && players::generation::OnGameThread())
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

void RegisterHudTraceCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"hud-trace",
                       .shape = control::Shape::Integer,
                       .min   = 1,
                       .max   = kMaxHudTraceFrames,
                       .help  = L"log the order of the HUD's tick, marker placements, icon projections and render for "
                                L"a number of frames, and which player each placement and projection used",
                       .set   = [](const Value& v) {
                           if (!RootOverlayRenderInstalled())
                           {
                               return Outcome{false, L"the overlay render hook is not installed (see the startup log)"};
                           }
                           // Installed by the first trace, not at every dev start (seven call redirects on the HUD's
                           // hot path).
                           if (!HudTraceInstalled())
                           {
                               const auto image = platform::GameImage::Locate();
                               if (!image || !InstallHudTrace(*image))
                               {
                                   return Outcome{false, L"the HUD trace could not be installed (see the log)"};
                               }
                           }
                           StartHudTrace(static_cast<std::int32_t>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"tracing (one log line per frame)"};
                       }});
}

}  // namespace lss::hud
