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
#include "hud/field/wrangle_targets.h"
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
#include "players/seats.h"

#if LSS_DEV
#include "hud/passes/second_image.h"
#include "platform/command_registry.h"
#endif

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

// New roots (markers the game adds) get the seats' contexts within this many frames.
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
constexpr const wchar_t* kPartNames[kParts] = {L"contexts to the further players", L"updates for the further players",
                                               L"contexts to player 1", L"updates for player 1", L"root scan"};

enum Lever : unsigned
{
    kLeverContexts    = 1,
    kLeverFirstPlayer = 2,
    kLeverPrimary     = 4,
    kLeverRect        = 8,
    kLeverUpdates     = 16,
    kLeverCompass     = 32,
    kLeverArrows      = 64,   // the off-screen arrows' own timers in each seat's view (hud/field/marker_timers.h)
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
using Rect = std::array<float, 4>;

std::atomic<bool>                                    g_on{false};
bool                                                 g_refused      = false;
std::uint32_t                                        g_framesToScan = 0;
bool                                                 g_rootsChecked = false;  // this frame's root scan check ran
bool                                                 g_passedThisFrame = false;  // a seat's pass ran this frame
std::optional<player_two::primary_controller::Scope> g_primary;
float                                                g_savedTimeStep = 0.0f;
float*                                               g_timeStep      = nullptr;  // saved in Enter, restored in Leave
Rect                                                 g_playerOneRect{};
// By seat ([0] unused): each seat's view rect as its last render computed it, and whether it is known yet.
std::array<Rect, players::kMaxPlayers>               g_seatRect{};
std::array<bool, players::kMaxPlayers>               g_seatRectKnown{};
std::array<bool, players::kMaxPlayers>               g_loggedEnter{};
struct FieldTargets
{
    std::size_t entries = 0, roots = 0, hiddenSwapped = 0;
    bool operator==(const FieldTargets&) const = default;
};
FieldTargets                                         g_fieldTargets;        // this frame's walk (hiddenSwapped unused)
FieldTargets                                         g_fieldTargetsLogged{static_cast<std::size_t>(-1)};
unsigned                                             g_mask               = 511;  // SetPlayerTwoLeversMask
// A pass left its seat's contexts for the next further pass (LeaveSeat with another): player 1's contexts and
// updates are due, from the next pass's LeaveSeat or SettleSeats.
bool                                                 g_playerOneDue       = false;
std::uint32_t                                        g_settledLate        = 0;  // since the census: SettleSeats did it
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
    platform::Log(L"HUD levers: {}; the further players' content stays off", why);
}

/// The updates for the view whose levers are set (`seat` 0 player 1's); a further seat's run gives the arrows their own
/// timers.
bool RunUpdates(int seat) noexcept
{
    const bool further = seat != 0;
    // The gauges first: the enemy attack names in the +0x180 list read each gauge's placed point
    // (RE-p2-battle-popups.md §1), so they must see this view's.
    if (Acts(kLeverGauges))
    {
        enemy_gauges::Replay(seat);
    }
    if (Acts(kLeverPopups))
    {
        popups::Replay(seat);
    }
    const hud_updates::OwnArrowTimers arrows{g_timeStep, g_savedTimeStep, seat};
    const bool ran = hud_updates::Run(Acts(kLeverUpdates), Acts(kLeverCompass),
                                      further && Acts(kLeverArrows) && g_timeStep != nullptr ? &arrows : nullptr);
    minimap::RunHolder(seat);  // the minimap's content for this view, while the seat's minimap is on
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
        hud_roots::ResetCensus();
        g_settledLate = 0;
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
    platform::LogDebug(L"HUD roots over {} frames: {}; player 1's put back outside a pass's own way back (a planned "
                       L"pass not run, or one without the levers) {} time(s)",
                       g_timedFrames, hud_roots::TakeCensus(g_timedFrames), g_settledLate);
    g_partSeconds.fill(0.0);
    g_settledLate = 0;
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

bool ReadRect(Rect& rect) noexcept
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

void WriteRect(const Rect& rect) noexcept
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

/// Rescans the HUD roots every kRootScanFrames frames, checked once a frame (the first further pass). False if a scan
/// failed (tried again by the next pass, this frame's or the next's) or the seat has no context (rescanned next frame).
bool KeepRootsCurrent(const hud_roots::LocalPlayerSet& bySeat, int seat)
{
    if (!g_rootsChecked)
    {
        if (g_framesToScan == 0)
        {
            const platform::frame_watch::Job job(L"HUD root scan");
            const double start   = Now();
            const bool   scanned = hud_roots::Scan(bySeat);
            Charge(kScan, start);
            if (!scanned)
            {
                return false;
            }
            g_framesToScan = kRootScanFrames;
        }
        --g_framesToScan;
        g_rootsChecked = true;
    }
    if (!hud_roots::HasContext(seat))
    {
        g_framesToScan = 0;  // the seat's context was not built: the next frame scans again
        return false;
    }
    return true;
}

/// L3: the seat as the party manager's primary controller. False (logged) if it cannot be held.
bool HoldPrimary(int seat)
{
    if (Acts(kLeverPrimary))
    {
        g_primary.emplace(seat);
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
/// collapsed by player 1's projection (an item off player 1's screen) must still get a further seat's context in the
/// seat's run, which shows it again from the seat's camera; player 1's re-run collapses it again for player 1's view.
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
/// a seat's context), and how many the last seat's context reached while collapsed. On change.
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
                       L"a further player's context (shown again if that player's camera sees them)",
                       now.entries, now.roots, now.hiddenSwapped);
}

/// L1 to the seat; on failure, puts player 1's contexts back and refuses.
bool ContextsToSeat(int seat)
{
    const double start    = Now();
    const bool   contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(seat);
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
                      L"a further player's context until the next join)");
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
        platform::Log(L"HUD levers: the HUD's update code is not what the research read; the further players' content "
                      L"unavailable");
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
        platform::Log(L"HUD levers: the first-player function's entry could not be redirected; the further players' "
                      L"content unavailable");
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
    g_framesToScan = 0;
    g_seatRectKnown.fill(false);
    g_loggedEnter.fill(false);
    platform::Log(L"HUD levers: the further players' content {}",
                  PlayerTwoContentOn() ? L"on" : on ? L"unavailable" : L"off");
}

void SetPlayerTwoLeversMask(unsigned mask) noexcept
{
    g_mask = mask;
    platform::Log(L"HUD levers: mask {} (contexts {}, first player {}, primary {}, rect {}, updates {}, compass {}, arrows {}, "
                  L"gauges {}, popups {})",
                  mask, Acts(kLeverContexts), Acts(kLeverFirstPlayer), Acts(kLeverPrimary), Acts(kLeverRect),
                  Acts(kLeverUpdates), Acts(kLeverCompass), Acts(kLeverArrows), Acts(kLeverGauges), Acts(kLeverPopups));
}

unsigned PlayerTwoLeversMask() noexcept
{
    return g_mask;
}

bool PlayerTwoContentOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void BeginPasses() noexcept
{
    if (g_passedThisFrame)
    {
        try
        {
            LogTiming();  // once per frame that had a pass, whatever the number of passes in it
        }
        catch (...)
        {
        }
    }
    g_rootsChecked    = false;
    g_passedThisFrame = false;
}

namespace {

/// The refusal for a failed way back to player 1 (LeaveSeat, SettleSeats). Allocates only for the log line.
void RefuseWayBack(bool contexts, bool ticks) noexcept
{
    try
    {
        if (!contexts || !ticks)
        {
            Refuse(!contexts ? L"SetPlayerContext raised an exception restoring player 1 (some roots may keep a "
                               L"further player's context until the next join)"
                             : L"a HUD update raised an exception in player 1's run");
        }
    }
    catch (...)
    {
        g_on.store(false, std::memory_order_relaxed);  // Refuse without its log line
    }
}

bool EnterSeatLevers(int seat) noexcept
{
    try
    {
        hud_roots::LocalPlayerSet bySeat{};
        if (!PlayerTwoContentOn() || !players::SeatIndex(seat) || !hud_roots::LocalPlayers(bySeat) ||
            bySeat[static_cast<std::size_t>(seat)] == nullptr || !KeepRootsCurrent(bySeat, seat) || !HoldPrimary(seat))
        {
            return false;
        }
        const auto s = static_cast<std::size_t>(seat);
        g_firstPlayerAnswer = Acts(kLeverFirstPlayer) ? reinterpret_cast<std::uint64_t>(bySeat[s]) : 0;  // L2
        AdoptAbilityNames();
        AdoptFieldTargets();
        search_pins::Adopt();
        if (!ContextsToSeat(seat))                                                                       // L1
        {
            return false;
        }
        LogFieldTargets();
        HoldTimeStep();  // L6
        ReadRect(g_playerOneRect);
        if (g_seatRectKnown[s] && Acts(kLeverRect))
        {
            WriteRect(g_seatRect[s]);  // what the seat's render computed last frame (via L2)
        }
        const double start = Now();
        const bool   runs  = RunUpdates(seat);
        search_pins::Place();         // the search pins from the seat's camera (hud/field/search_pins.h)
        wrangle_targets::Place(seat);  // and the chocobo wrangle's targets (hud/field/wrangle_targets.h)
        Charge(kTicksTwo, start);
        if (!runs)
        {
            Refuse(L"a HUD update raised an exception in a further player's run");
        }
        if (!g_loggedEnter[s])
        {
            g_loggedEnter[s] = true;
            platform::Log(L"HUD levers: {}'s pass with {} HUD root(s); {}'s view rect {}", players::SeatName(seat),
                          hud_roots::Count(), players::SeatName(seat),
                          g_seatRectKnown[s] ? L"from the last frame" : L"not known yet (first frame)");
        }
        return true;
    }
    catch (...)
    {
        // Only the log lines allocate; whatever was switched before the throw is put back.
        LeaveSeat(seat, false);
        g_on.store(false, std::memory_order_relaxed);
        return false;
    }
}

}  // namespace

bool EnterSeat(int seat) noexcept
{
    const bool entered = EnterSeatLevers(seat);
    if (!entered)
    {
        SettleSeats();  // the pass renders player 1's content: an earlier pass's contexts and positions put back
    }
    return entered;
}

void LeaveSeat(int seat, bool another) noexcept
{
    // Every lever is put back whatever happens to the others: each step is guarded on its own
    // (the engine calls by SEH inside the helpers), and nothing here allocates except the log.
    // With another further pass to come, the roots' contexts and player 1's re-run wait for the
    // last pass: the next seat's own contexts and updates replace this one's, and the roots go
    // back to player 1 once (hud/context_owners.h).
    if (players::SeatIndex(seat))
    {
        const auto s      = static_cast<std::size_t>(seat);
        g_seatRectKnown[s] = ReadRect(g_seatRect[s]);  // computed by the seat's render through L2
    }
    g_firstPlayerAnswer = 0;
    double mark         = Now();
    bool   contexts     = true;
    if (!another)
    {
        contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(0);
        mark     = Charge(kContextsOne, mark);
    }
    g_primary.reset();  // SwapWindow's close: a no-op unless it opened
    WriteRect(g_playerOneRect);
    bool ticks = true;
    mark       = Now();
    if (!another)
    {
        ticks = RunUpdates(0);      // player 1's positions again, before player 1's render
        search_pins::Place();       // and the search pins'
        wrangle_targets::Place(0);  // and the chocobo wrangle's targets'
    }
    else
    {
        // The minimap's holder back to player 1's: the next seat's run skips it while that seat's minimap is off
        // (just after a join), and its pass would paint this seat's map.
        minimap::RunHolder(0);
    }
    Charge(kTicksOne, mark);
    RestoreTimeStep();
    g_playerOneDue    = another;
    g_passedThisFrame = true;
    RefuseWayBack(contexts, ticks);
}

void SettleSeats() noexcept
{
    if (!g_playerOneDue)
    {
        return;
    }
    g_playerOneDue = false;
    ++g_settledLate;
    HoldTimeStep();  // L6 for player 1's re-run, as inside a pass
    double     mark     = Now();
    const bool contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(0);
    mark                = Charge(kContextsOne, mark);
    const bool ticks    = RunUpdates(0);
    search_pins::Place();
    wrangle_targets::Place(0);
    Charge(kTicksOne, mark);
    RestoreTimeStep();
    RefuseWayBack(contexts, ticks);
}

bool PlayerOneDue() noexcept
{
    return g_playerOneDue;
}

#if LSS_DEV

void RegisterPlayerTwoPassCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"hud-levers",
         .shape = control::Shape::OnOff,
         .help  = L"each further player's image shows their own content: the game's player levers switched around "
                  L"their pass",
         .set   = [](const Value& v) {
             if (!PlayerTwoPassInstalled())
             {
                 return Outcome{false, L"the HUD levers are not installed (see the startup log)"};
             }
             const bool on = std::get<bool>(v);
             SetPlayerTwoContent(on);
             if (on && !PlayerTwoContentOn())
             {
                 return Outcome{false, L"the HUD levers refused a check (see the log)"};
             }
             return Outcome{true, !on              ? L"the further players' content off"
                                  : SecondImageOn() ? L"the further players' content on"
                                                    : L"the further players' content on (needs hud-image on)"};
         },
         .get = [] { return Value{PlayerTwoContentOn()}; }});
    control::Register(
        {.name  = L"hud-levers-mask",
         .shape = control::Shape::Integer,
         .max   = 511,
         .help  = L"diagnosis: which levers act (1 contexts, 2 first player, 4 primary controller, 8 view rect, 16 "
                  L"updates, 32 compass, 64 the arrows' own timers, 128 the enemy gauges per view, 256 the battle "
                  L"popups per view)",
         .set   = [](const Value& v) {
             SetPlayerTwoLeversMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
             return Outcome{true, L"levers mask set (see the log)"};
         },
         .get = [] { return Value{std::int64_t{PlayerTwoLeversMask()}}; }});
}

#endif

}  // namespace lss::hud
