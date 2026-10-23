#include "hud/passes/player_two_pass.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>

#include "facts/hud.h"
#include "hud/battle/enemy_gauges.h"
#include "hud/battle/popups.h"
#include "hud/field/marker_timers.h"
#include "hud/field/search_pins.h"
#include "hud/hud_roots.h"
#include "hud/hud_updates.h"
#include "hud/minimap/minimap.h"
#include "hud/passes/first_player_code.h"
#include "hud/user_widgets.h"
#include "platform/code_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/sparse_array.h"
#include "player_two/scopes/primary_controller.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace lss::hud {

namespace {

using RC::Unreal::UObject;

// Facts read from the database (RE-p2-hud.md pass 9, pass 12).
constexpr std::uintptr_t kGameMode            = 0x1490FF2B8;  // g_AEndGameMode (pointer)
constexpr std::uint32_t  kGameModeHudTimeStep = 0x5D8;        // float, written once per engine tick

// GEngine__GetFirstGamePlayerForGameViewport's first instruction (hud/passes/first_player_code.h).
constexpr std::uintptr_t kFirstGamePlayer       = 0x140CCF8E4;
constexpr std::uintptr_t kFirstGamePlayerResume = 0x140CCF8E9;

// The enemy ability-name labels (RE-p2-battle-popups.md §2.1): a TSparseArray at *(EndUISystem+0x180),
// 72-byte elements, the label's weak widget at +8. A new label is made for each name shown.
constexpr std::uint32_t  kAbilityNames      = 0x180;
constexpr std::uint32_t  kAbilityNameEntry  = 0x48;
constexpr std::uint32_t  kAbilityNameWidget = 0x8;
// The field-target markers (RE-p2-field-interact.md §12.14): EndUISystem+0x98 points to a TSparseArray of 140-byte
// elements; weak widgets at +0x00 (projected), +0x28 (placed at the point), +0x30 (animated).
constexpr std::uint32_t  kFieldTargetWidgets[] = {0x00, 0x28, 0x30};

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

enum Lever : unsigned
{
    kLeverContexts    = 1,
    kLeverFirstPlayer = 2,
    kLeverPrimary     = 4,
    kLeverRect        = 8,
    kLeverUpdates     = 16,
    kLeverCompass     = 32,
    kLeverArrows      = 64,   // the off-screen arrows' own timers in player 2's view (hud/field/marker_timers.h)
    kLeverGauges      = 128,  // the enemy gauges placed again per view (hud/battle/enemy_gauges.h)
    kLeverPopups      = 256,  // the battle popups placed again per view (hud/battle/popups.h)
};

// Installed once, then read.
platform::CodeRedirect g_firstPlayerCode;
bool                   g_installed = false;
std::uintptr_t         g_gameMode  = 0;
std::uintptr_t         g_overlay   = 0;  // the singleton's address
std::uintptr_t         g_endUISystem = 0;

// Read by the first-player code through its address.
alignas(8) std::uint64_t g_firstPlayerAnswer = 0;

// Game thread.
std::atomic<bool>                                    g_on{false};
bool                                                 g_refused      = false;
std::uint32_t                                        g_framesToScan = 0;
std::optional<player_two::primary_controller::Scope> g_primary;
float                                                g_savedTimeStep = 0.0f;
float*                                               g_timeStep      = nullptr;  // saved in Enter, restored in Leave
std::array<float, 4>                                 g_playerOneRect{};
std::array<float, 4>                                 g_playerTwoRect{};
bool                                                 g_playerTwoRectKnown = false;
bool                                                 g_loggedEnter        = false;
struct FieldTargets
{
    std::size_t entries = 0, roots = 0, hiddenSwapped = 0;
    bool operator==(const FieldTargets&) const = default;
};
FieldTargets                                         g_fieldTargets;        // this frame's walk (hiddenSwapped unused)
FieldTargets                                         g_fieldTargetsLogged{static_cast<std::size_t>(-1)};
unsigned                                             g_mask               = 511;  // SetPlayerTwoLeversMask
std::array<double, kParts>                           g_partSeconds{};
std::uint32_t                                        g_timedFrames     = 0;

bool Acts(Lever lever) noexcept
{
    return (g_mask & lever) != 0;
}

using platform::frame_watch::Now;

/// Adds the time since `start` to `part`, and returns now.
double Charge(Part part, double start) noexcept
{
    const double now = Now();
    g_partSeconds[part] += now - start;
    return now;
}

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD levers: {}; player 2's content stays off", why);
}

/// The updates for the view whose levers are set; player 2's run gives the arrows their own timers.
bool RunUpdates(bool playerTwo) noexcept
{
    // The gauges first: the enemy attack names in the +0x180 list read each gauge's placed point
    // (RE-p2-battle-popups.md §1), so they must see this view's.
    if (Acts(kLeverGauges))
    {
        enemy_gauges::Replay(playerTwo);
    }
    if (Acts(kLeverPopups))
    {
        popups::Replay(playerTwo);
    }
    const hud_updates::OwnArrowTimers arrows{g_timeStep, g_savedTimeStep};
    const bool ran = hud_updates::Run(Acts(kLeverUpdates), Acts(kLeverCompass),
                                      playerTwo && Acts(kLeverArrows) && g_timeStep != nullptr ? &arrows : nullptr);
    minimap::RunHolder(playerTwo);  // the minimap's content for this view, while player 2's minimap is on
    return ran;
}

void LogTiming()
{
    if (++g_timedFrames < kTimingFrames)
    {
        return;
    }
    if (!platform::frame_watch::Detailed())  // the perf-log switch: counts start afresh, unlogged
    {
        g_partSeconds.fill(0.0);
        hud_updates::TakeTimings(g_timedFrames);
        g_timedFrames = 0;
        return;
    }
    const platform::frame_watch::Job job(L"HUD levers timing log");
    const double perFrame = 1000.0 / g_timedFrames;
    std::wstring parts;
    for (std::size_t i = 0; i < kParts; ++i)
    {
        parts += std::format(L"{}{} {:.3f}", i == 0 ? L"" : L", ", kPartNames[i], g_partSeconds[i] * perFrame);
    }
    platform::Log(L"HUD levers: ms per frame over {} frames: {}; updates (both runs):{}; {} user widgets live", g_timedFrames,
                  parts, hud_updates::TakeTimings(g_timedFrames), user_widgets::Live().size());
    g_partSeconds.fill(0.0);
    g_timedFrames = 0;
}

float* TimeStepField() noexcept
{
    std::uintptr_t mode = 0;
    if (!platform::CopyGuarded(&mode, reinterpret_cast<const void*>(g_gameMode), sizeof(mode)) || mode == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<float*>(mode + kGameModeHudTimeStep);
}

std::uintptr_t Overlay() noexcept
{
    std::uintptr_t overlay = 0;
    platform::CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlay), sizeof(overlay));
    return overlay;
}

bool ReadRect(std::array<float, 4>& rect) noexcept
{
    const std::uintptr_t overlay = Overlay();
    for (std::size_t i = 0; overlay != 0 && i < rect.size(); ++i)
    {
        if (!platform::CopyGuarded(&rect[i], reinterpret_cast<const void*>(overlay + kOverlayRectFields[i]), sizeof(float)))
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
        platform::WriteGuarded(reinterpret_cast<void*>(overlay + kOverlayRectFields[i]), &rect[i], sizeof(float));
    }
}

/// L6: the game mode's HUD time step to 0 for the re-runs, so timers do not advance twice.
void HoldTimeStep() noexcept
{
    float* step = TimeStepField();
    if (step == nullptr)
    {
        return;
    }
    const float still = 0.0f;
    if (platform::CopyGuarded(&g_savedTimeStep, step, sizeof(float)))
    {
        g_timeStep = step;
        platform::WriteGuarded(step, &still, sizeof(float));
    }
}

void RestoreTimeStep() noexcept
{
    if (g_timeStep != nullptr)
    {
        platform::WriteGuarded(g_timeStep, &g_savedTimeStep, sizeof(float));
        g_timeStep = nullptr;
    }
}

/// Rescans the HUD roots every kRootScanFrames frames. False if a scan failed (retried next frame).
bool KeepRootsCurrent(UObject* playerOne, UObject* playerTwo)
{
    if (g_framesToScan == 0)
    {
        const platform::frame_watch::Job job(L"HUD root scan");
        const double start   = Now();
        const bool   scanned = hud_roots::Scan(playerOne, playerTwo);
        Charge(kScan, start);
        if (!scanned)
        {
            return false;
        }
        g_framesToScan = kRootScanFrames;
    }
    --g_framesToScan;
    return true;
}

/// L3: player 2 as the party manager's primary controller. False (logged) if it cannot be held.
bool HoldPrimary()
{
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
    return true;
}

/// Every enemy ability-name label becomes a root at once: each is a new widget, and the root scan
/// runs only every kRootScanFrames, so a label would otherwise start with player 1's context.
void AdoptAbilityNames()
{
    std::uintptr_t map = 0;
    if (g_endUISystem == 0 || !platform::CopyGuarded(&map, reinterpret_cast<const void*>(g_endUISystem + kAbilityNames),
                                                     sizeof(map)))
    {
        return;
    }
    platform::ForEachSparseElement(map, kAbilityNameEntry, [](std::uintptr_t entry) {
        RC::Unreal::FWeakObjectPtr weak;
        std::uint64_t              value = 0;
        if (platform::CopyGuarded(&value, reinterpret_cast<const void*>(entry + kAbilityNameWidget), sizeof(value)) && value != 0)
        {
            std::memcpy(&weak, &value, sizeof(value));
            hud_roots::Adopt(weak.Get());
        }
        return true;
    });
}

/// The field-target markers (EndUISystem+0x98; one per field-action point, RE-p2-field-interact.md §12.14) are
/// self-placing roots: their tick projects each through its own context and collapses it when that fails, so one
/// collapsed by player 1's projection (an item off player 1's screen) must still get player 2's context in player
/// 2's run, which shows it again from player 2's camera; player 1's re-run collapses it again for player 1's half.
/// The widget at +0x00 is the one projected; +0x28 and +0x30 are placed and animated from it. Logged on change.
void AdoptFieldTargets()
{
    std::uintptr_t list = 0;
    const auto markers = reinterpret_cast<const void*>(g_endUISystem + facts::kUIFieldMarkers);
    if (g_endUISystem == 0 || !platform::CopyGuarded(&list, markers, sizeof(list)))
    {
        return;
    }
    std::size_t entries = 0, roots = 0;
    platform::ForEachSparseElement(list, facts::kFieldMarkerEntry, [&entries, &roots](std::uintptr_t entry) {
        ++entries;
        for (const std::uint32_t at : kFieldTargetWidgets)
        {
            RC::Unreal::FWeakObjectPtr weak;
            std::uint64_t              value = 0;
            if (platform::CopyGuarded(&value, reinterpret_cast<const void*>(entry + at), sizeof(value)) && value != 0)
            {
                std::memcpy(&weak, &value, sizeof(value));
                roots += hud_roots::AdoptSelfPlacing(weak.Get()) && at == kFieldTargetWidgets[0] ? 1 : 0;
            }
        }
        return true;
    });
    g_fieldTargets = {entries, roots};
}

/// The field-target markers' line: how many there are, how many are roots (a marker that is not one never gets
/// player 2's context), and how many player 2's context reached while collapsed. On change.
void LogFieldTargets()
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const FieldTargets now{g_fieldTargets.entries, g_fieldTargets.roots, hud_roots::HiddenSwapped()};
    if (now == g_fieldTargetsLogged)
    {
        return;
    }
    g_fieldTargetsLogged = now;
    platform::LogDebug(L"HUD levers: field-target markers: {} in the list, {} of them roots; {} collapsed root(s) given "
                       L"player 2's context (shown again if player 2's camera sees them)",
                       now.entries, now.roots, now.hiddenSwapped);
}

/// L1 to player 2; on failure, puts player 1's contexts back and refuses.
bool ContextsToPlayerTwo()
{
    const double start    = Now();
    const bool   contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(1);
    Charge(kContextsTwo, start);
    if (contexts)
    {
        return true;
    }
    g_firstPlayerAnswer = 0;
    const bool reverted = hud_roots::SetContexts(0);
    g_primary.reset();
    Refuse(reverted ? L"SetPlayerContext raised an exception"
                    : L"SetPlayerContext raised an exception, and again putting player 1's back (some roots may keep "
                      L"player 2's context until the next join)");
    return false;
}

}  // namespace

bool InstallPlayerTwoPass(const platform::GameImage& image) noexcept
{
    const bool parts   = hud_roots::Install(image) && hud_updates::Install(image) && marker_timers::Install(image);
    enemy_gauges::Install(image);  // optional: without it the gauges keep player 1's placement in both halves
    popups::Install(image);      // optional, likewise for the battle popups
    const auto mode    = image.Rebase(kGameMode, sizeof(void*));
    const auto overlay = image.Rebase(facts::kRootOverlaySingleton, sizeof(void*));
    const auto entry   = image.Rebase(kFirstGamePlayer, sizeof(kFirstPlayerEntryBytes));
    const auto resume  = image.Rebase(kFirstGamePlayerResume);
    if (!parts || !mode || !overlay || !entry || !resume)
    {
        platform::Log(L"HUD levers: the HUD's update code is not what the research read; player 2's content unavailable");
        return false;
    }
    g_gameMode = *mode;
    g_overlay  = *overlay;
    const auto system = image.Rebase(facts::kEndUISystem, kAbilityNames + sizeof(void*));
    g_endUISystem     = system ? *system : 0;

    // The first-player code reads g_firstPlayerAnswer, in this DLL: pin it before installing.
    const auto code = FirstPlayerCode(g_firstPlayerAnswer);
    if (!platform::PinThisModule() || !g_firstPlayerCode.Install(*entry, kFirstPlayerEntryBytes, code, *resume))
    {
        platform::Log(L"HUD levers: the first-player function's entry could not be redirected; player 2's content "
                      L"unavailable");
        return false;
    }
    g_installed = true;
    g_on.store(!g_refused, std::memory_order_relaxed);  // on from the start; the hud-levers command switches it
    return true;
}

void RemovePlayerTwoPass() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_firstPlayerAnswer = 0;
    enemy_gauges::Remove();
    popups::Remove();
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
    platform::Log(L"HUD levers: mask {} (contexts {}, first player {}, primary {}, rect {}, updates {}, compass {}, arrows {}, "
                  L"gauges {}, popups {})",
                  mask, Acts(kLeverContexts), Acts(kLeverFirstPlayer), Acts(kLeverPrimary), Acts(kLeverRect),
                  Acts(kLeverUpdates), Acts(kLeverCompass), Acts(kLeverArrows), Acts(kLeverGauges), Acts(kLeverPopups));
}

bool PlayerTwoContentOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

bool EnterPlayerTwo() noexcept
{
    try
    {
        UObject* playerOne = nullptr;
        UObject* playerTwo = nullptr;
        if (!PlayerTwoContentOn() || !hud_roots::LocalPlayers(playerOne, playerTwo) ||
            !KeepRootsCurrent(playerOne, playerTwo) || !HoldPrimary())
        {
            return false;
        }
        g_firstPlayerAnswer = Acts(kLeverFirstPlayer) ? reinterpret_cast<std::uint64_t>(playerTwo) : 0;  // L2
        AdoptAbilityNames();
        AdoptFieldTargets();
        search_pins::Adopt();
        if (!ContextsToPlayerTwo())                                                                       // L1
        {
            return false;
        }
        LogFieldTargets();
        HoldTimeStep();  // L6
        ReadRect(g_playerOneRect);
        if (g_playerTwoRectKnown && Acts(kLeverRect))
        {
            WriteRect(g_playerTwoRect);  // what player 2's render computed last frame (via L2)
        }
        const double start = Now();
        const bool   runs  = RunUpdates(true);
        search_pins::Place();  // the search pins from player 2's camera (hud/field/search_pins.h)
        Charge(kTicksTwo, start);
        if (!runs)
        {
            Refuse(L"a HUD update raised an exception in player 2's run");
        }
        if (!g_loggedEnter)
        {
            g_loggedEnter = true;
            platform::Log(L"HUD levers: player 2's pass with {} HUD root(s); player 2's view rect {}", hud_roots::Count(),
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
    const bool contexts  = !Acts(kLeverContexts) || hud_roots::SetContexts(0);
    mark                 = Charge(kContextsOne, mark);
    g_primary.reset();  // SwapWindow's close: a no-op unless it opened
    WriteRect(g_playerOneRect);
    mark             = Now();
    const bool ticks = RunUpdates(false);  // player 1's positions again, before player 1's render
    search_pins::Place();                  // and the search pins'
    Charge(kTicksOne, mark);
    RestoreTimeStep();
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
