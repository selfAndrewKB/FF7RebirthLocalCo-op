#include "hud/passes/seat_hud_content.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>

#include "facts/hud.h"
#include "hud/battle/enemy_gauges.h"
#include "hud/battle/popups.h"
#include "hud/field/jet_gauges.h"
#include "hud/field/marker_timers.h"
#include "hud/field/search_pins.h"
#include "hud/field/wrangle_targets.h"
#include "hud/hud_roots.h"
#include "hud/hud_updates.h"
#include "hud/minimap/minimap.h"
#include "hud/passes/first_player_code.h"
#include "hud/passes/overlay_pass.h"
#include "hud/user_widgets.h"
#include "platform/code_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/sparse_array.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/primary_controller.h"

#if LSS_DEV
#include "hud/passes/column_hud_images.h"
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace lss::hud {

namespace {

using RC::Unreal::UObject;

// Facts read from the database (research/RE-p2-hud.md).
constexpr std::uintptr_t kGameMode            = 0x1490FF2B8;  // g_AEndGameMode (pointer)
constexpr std::uint32_t  kGameModeHudTimeStep = 0x5D8;        // float, written once per engine tick

// GEngine__GetFirstGamePlayerForGameViewport's first instruction (hud/passes/first_player_code.h).
constexpr std::uintptr_t kFirstGamePlayer       = 0x140CCF8E4;
constexpr std::uintptr_t kFirstGamePlayerResume = 0x140CCF8E9;

// The enemy ability-name labels (research/RE-p2-battle-popups.md): a TSparseArray at *(EndUISystem+0x180),
// 72-byte elements, the label's weak widget at +8. A new label is made for each name shown.
constexpr std::uint32_t  kAbilityNames      = 0x180;
constexpr std::uint32_t  kAbilityNameEntry  = 0x48;
constexpr std::uint32_t  kAbilityNameWidget = 0x8;
// The field-target markers (research/RE-p2-field-interact.md): EndUISystem+0x98 points to a TSparseArray of 140-byte
// elements; weak widgets at +0x00 (projected), +0x28 (placed at the point), +0x30 (animated).
constexpr std::uint32_t  kFieldTargetWidgets[] = {0x00, 0x28, 0x30};

/// The overlay's marker host set (facts/hud.h) as it lies, read and written in one guarded copy. Each render writes it for
/// the view it renders, and a column's updates run before that column's render, so they would read the set the render
/// before them left: another view's, wrong wherever the views differ in shape (three players: player 1's wide band and
/// two narrow views). Each pass's updates get their column's set, as its render last wrote it, and player 1's comes back
/// for player 1's re-run.
struct HostValues
{
    float min[2] = {}, origin[2] = {}, ui[2] = {}, size[2] = {}, scale = 0.0f;
};
static_assert(sizeof(HostValues) == facts::kOverlayHostScale + sizeof(float) - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, origin) == facts::kOverlayViewOrigin - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, ui) == facts::kOverlayUiSize - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, size) == facts::kOverlayViewWidth - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, scale) == facts::kOverlayHostScale - facts::kOverlayLayoutMin);

// New roots (markers the game adds) get the seats' contexts within this many frames.
constexpr std::uint32_t kRootScanFrames = 30;
constexpr std::uint32_t kTimingFrames   = 240;

/// What each part of the pass costs, for the periodic log line.
enum Part : std::size_t
{
    kContextsSeat,
    kTicksSeat,
    kContextsOne,
    kTicksOne,
    kScan,
    kParts,
};
constexpr const wchar_t* kPartNames[kParts] = {L"contexts to players 2 and 3", L"updates for players 2 and 3",
                                               L"contexts to player 1", L"updates for player 1", L"root scan"};

enum Lever : unsigned
{
    kLeverContexts    = 1,
    kLeverFirstPlayer = 2,
    kLeverPrimary     = 4,
    kLeverHostSet     = 8,    // each column's marker host set for its updates (HostValues)
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
std::atomic<bool>                                   g_on{false};
bool                                                 g_refused      = false;
std::uint32_t                                        g_framesToScan = 0;
bool                                                 g_rootsChecked = false;  // this frame's root scan check ran
bool                                                 g_passedThisFrame = false;  // a seat's pass ran this frame
std::optional<scopes::primary_controller::Scope> g_primary;
void* g_columnController = nullptr;  // the controller of the column whose levers are set (EnterSeat to LeaveSeat)
int   g_holderShown      = 0;        // whose map the minimap's holder last ran for (0 player 1's)
int g_mirrorPrimaryHeld = -1;  // a mirror's copy held its controller as the primary last time: 1, 0, -1 not yet
float                                                g_savedTimeStep = 0.0f;
float*                                               g_timeStep      = nullptr;  // saved in Enter, restored in Leave
// Player 1's set, saved once a frame before the passes (player 1's render wrote it last) and put back after each
// further column's render, whether its levers held or not, so no later pass or re-run reads a column's.
HostValues                                           g_playerOneHostSet{};
bool                                                 g_playerOneHostSetSaved = false;
// By column ([0] unused): each further column's host set as its last render wrote it; known only while that column
// rendered with its levers every frame since and the columns kept their hands (a skipped frame or a join, leave or
// mirror change forgets it: the first pass after one runs with player 1's set, as before any pass).
std::array<HostValues, overlay_pass::kMaxColumns>    g_columnHostSet{};
std::array<bool, overlay_pass::kMaxColumns>          g_columnHostSetKnown{};
std::array<bool, overlay_pass::kMaxColumns>          g_columnHostSetRead{};  // read back this frame
views::columns::Map                                  g_hostSetMap;  // the columns the sets were read for
std::array<bool, overlay_pass::kMaxColumns>          g_loggedEnter{};
views::columns::Map                                  g_scannedMap;  // the columns the roots' contexts were built for
struct FieldTargets
{
    std::size_t entries = 0, roots = 0, hiddenSwapped = 0;
    bool operator==(const FieldTargets&) const = default;
};
FieldTargets                                         g_fieldTargets;        // this frame's walk (hiddenSwapped unused)
FieldTargets                                         g_fieldTargetsLogged{static_cast<std::size_t>(-1)};
unsigned                                             g_mask               = 511;  // SetSeatLeversMask
// A pass left its seat's contexts for the next seat's pass (LeaveSeat with another): player 1's contexts and
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
    platform::Log(L"HUD levers: {}; players 2 and 3's content stays off", why);
}

/// Who a pass's updates are for: the column (0 player 1's re-run), its player (`seat`, 0 a mirror's: whose view the
/// popups project into and whose arrow timers run, `controller` then the mirror's) and whose content shows (`shown`,
/// 0 player 1's: a copy).
struct UpdatesFor
{
    int   column     = 0;
    int   seat       = 0;
    int   shown      = 0;
    void* controller = nullptr;
};

/// The updates for the view whose levers are set; a further column's run gives the arrows their own timers.
bool RunUpdates(const UpdatesFor& pass) noexcept
{
    // The gauges first: the enemy attack names in the +0x180 list read each gauge's placed point
    // (research/RE-p2-battle-popups.md), so they must see this view's. Classified and sized by the shown player's target.
    if (Acts(kLeverGauges))
    {
        enemy_gauges::Replay(pass.shown);
    }
    if (Acts(kLeverPopups))
    {
        popups::Replay(pass.seat, pass.seat == 0 ? pass.controller : nullptr);
    }
    const hud_updates::OwnArrowTimers arrows{g_timeStep, g_savedTimeStep, pass.seat, pass.column};
    const bool ran = hud_updates::Run(Acts(kLeverUpdates), Acts(kLeverCompass),
                                      pass.column >= 1 && Acts(kLeverArrows) && g_timeStep != nullptr ? &arrows : nullptr);
    minimap::RunHolder(pass.shown);  // the minimap's content for the shown player (player 1's: back to theirs if changed)
    g_holderShown = pass.shown;
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

bool ReadHostSet(HostValues& set) noexcept
{
    const std::uintptr_t overlay = Overlay();
    return overlay != 0 &&
           platform::CopyGuarded(&set, reinterpret_cast<const void*>(overlay + facts::kOverlayLayoutMin), sizeof(set));
}

void WriteHostSet(const HostValues& set) noexcept
{
    if (const std::uintptr_t overlay = Overlay(); overlay != 0)
    {
        platform::WriteGuarded(reinterpret_cast<void*>(overlay + facts::kOverlayLayoutMin), &set, sizeof(set));
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

/// Rescans the HUD roots every kRootScanFrames frames, and at once when the columns changed (a join, a leave, a mirror
/// in or out: a column's context would name its former player), checked once a frame (the first further pass). False
/// if a scan failed (tried again by the next pass, this frame's or the next's) or the column has no context (rescanned
/// next frame).
bool KeepRootsCurrent(const hud_roots::LocalPlayerSet& byColumn, int column)
{
    if (!g_rootsChecked)
    {
        if (overlay_pass::Map() != g_scannedMap)
        {
            g_framesToScan = 0;
        }
        if (g_framesToScan == 0)
        {
            const platform::frame_watch::Job job(L"HUD root scan");
            const double start   = Now();
            const bool   scanned = hud_roots::Scan(byColumn);
            Charge(kScan, start);
            if (!scanned)
            {
                return false;
            }
            g_framesToScan = kRootScanFrames;
            g_scannedMap   = overlay_pass::Map();
        }
        --g_framesToScan;
        g_rootsChecked = true;
    }
    if (!hud_roots::HasContext(column))
    {
        g_framesToScan = 0;  // the column's context was not built: the next frame scans again
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
                      scopes::primary_controller::Describe(g_primary->Why()));
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

/// The field-target markers (EndUISystem+0x98; one per field-action point, research/RE-p2-field-interact.md) are
/// self-placing roots: their tick projects each through its own context and collapses it when that fails, so one
/// collapsed by player 1's projection (an item off player 1's screen) must still get a seat's context in the
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
                       L"player 2's or 3's context (shown again if that player's camera sees them)",
                       now.entries, now.roots, now.hiddenSwapped);
}

/// L1 to the column's player (the seat's, `seat`, for the log); on failure, puts player 1's contexts back and refuses.
bool ContextsToSeat(int column, int seat)
{
    const double start    = Now();
    const bool   contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(column);
    Charge(kContextsSeat, start);
    if (contexts)
    {
        return true;
    }
    g_firstPlayerAnswer = 0;
    const bool reverted = hud_roots::SetContexts(0);
    g_primary.reset();
    Refuse(reverted    ? L"SetPlayerContext raised an exception"
           : seat == 0 ? L"SetPlayerContext raised an exception, and again putting player 1's back (some roots may keep "
                         L"a mirror view's context until the next join)"
                       : players::BySeat(seat,
                                      L"SetPlayerContext raised an exception, and again putting player 1's back (some "
                                      L"roots may keep player 2's context until the next join)",
                                      L"SetPlayerContext raised an exception, and again putting player 1's back (some "
                                      L"roots may keep player 3's context until the next join)"));
    return false;
}

}  // namespace

bool InstallSeatHudContent(const platform::GameImage& image) noexcept
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
        platform::Log(L"HUD levers: the HUD's update code is not what the research read; players 2 and 3's content "
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
        platform::Log(L"HUD levers: the first-player function's entry could not be redirected; players 2 and 3's "
                      L"content unavailable");
        return false;
    }
    g_installed = true;
    g_on.store(!g_refused, std::memory_order_relaxed);  // on from the start; the hud-levers command switches it
    return true;
}

void RemoveSeatHudContent() noexcept
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

bool SeatHudContentInstalled() noexcept
{
    return g_installed;
}

void SetSeatHudContent(bool on) noexcept
{
    g_on.store(on && g_installed && !g_refused, std::memory_order_relaxed);
    g_framesToScan = 0;
    g_columnHostSetKnown.fill(false);
    g_loggedEnter.fill(false);
    platform::Log(L"HUD levers: players 2 and 3's content {}",
                  SeatHudContentOn() ? L"on" : on ? L"unavailable" : L"off");
}

void SetSeatLeversMask(unsigned mask) noexcept
{
    g_mask = mask;
    platform::Log(L"HUD levers: mask {} (contexts {}, first player {}, primary {}, host set {}, updates {}, compass {}, arrows {}, "
                  L"gauges {}, popups {})",
                  mask, Acts(kLeverContexts), Acts(kLeverFirstPlayer), Acts(kLeverPrimary), Acts(kLeverHostSet),
                  Acts(kLeverUpdates), Acts(kLeverCompass), Acts(kLeverArrows), Acts(kLeverGauges), Acts(kLeverPopups));
}

unsigned SeatLeversMask() noexcept
{
    return g_mask;
}

bool SeatHudContentOn() noexcept
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
    // A column that did not render with its levers last frame, or any column after the columns changed, has no set.
    const bool sameColumns = overlay_pass::Map() == g_hostSetMap;
    g_hostSetMap           = overlay_pass::Map();
    for (std::size_t c = 0; c < g_columnHostSetKnown.size(); ++c)
    {
        g_columnHostSetKnown[c] = sameColumns && g_columnHostSetKnown[c] && g_columnHostSetRead[c];
    }
    g_columnHostSetRead.fill(false);
    g_playerOneHostSetSaved = ReadHostSet(g_playerOneHostSet);
}

void PutBackPlayerOneHostSet() noexcept
{
    if (g_playerOneHostSetSaved)
    {
        WriteHostSet(g_playerOneHostSet);
    }
}

namespace {

/// LeaveSeat's way back; `rendered` false (a throw before the render) leaves the column's set unread.
void LeaveLevers(int column, bool another, bool rendered) noexcept;

/// The refusal for a failed way back to player 1 (LeaveSeat, SettleSeats). Allocates only for the log line.
void RefuseWayBack(bool contexts, bool ticks) noexcept
{
    try
    {
        if (!contexts || !ticks)
        {
            Refuse(!contexts ? L"SetPlayerContext raised an exception restoring player 1 (some roots may keep "
                               L"player 2's or 3's context until the next join)"
                             : L"a HUD update raised an exception in player 1's run");
        }
    }
    catch (...)
    {
        g_on.store(false, std::memory_order_relaxed);  // Refuse without its log line
    }
}

bool EnterSeatLevers(int column, int seat, bool copy) noexcept
{
    try
    {
        // `seat` 0: a copy of player 1's content in a column no seat has (a mirror's): the primary controller stays
        // player 1's through the updates; the popups project with the column's own controller, and the chocobo
        // wrangle's placement, which projects through the primary controller, runs with the mirror's held for it alone.
        hud_roots::LocalPlayerSet byColumn{};
        if (!SeatHudContentOn() || (seat != 0 && !players::SeatIndex(seat)) || column < 1 ||
            column >= overlay_pass::kMaxColumns || !hud_roots::LocalPlayers(byColumn) ||
            byColumn[static_cast<std::size_t>(column)] == nullptr || !KeepRootsCurrent(byColumn, column) ||
            (seat != 0 && !HoldPrimary(seat)))
        {
            return false;
        }
        const auto c          = static_cast<std::size_t>(column);
        void*      controller = seat != 0 ? players::generation::ControllerOf(seat)
                                          : players::unreal::LocalPlayerController(byColumn[c]);
        void*      playerOne  = seat == 0 ? players::unreal::LocalPlayerController(byColumn[0]) : nullptr;
        if (seat == 0 && (controller == nullptr || playerOne == nullptr))
        {
            return false;
        }
        g_columnController = controller;
        g_firstPlayerAnswer = Acts(kLeverFirstPlayer) ? reinterpret_cast<std::uint64_t>(byColumn[c]) : 0;  // L2
        AdoptAbilityNames();
        AdoptFieldTargets();
        search_pins::Adopt();
        if (!ContextsToSeat(column, seat))                                                                 // L1
        {
            return false;
        }
        LogFieldTargets();
        HoldTimeStep();  // L6
        if (g_playerOneHostSetSaved && g_columnHostSetKnown[c] && Acts(kLeverHostSet))
        {
            WriteHostSet(g_columnHostSet[c]);  // what the column's render wrote last frame (its view through L2, its band)
        }
        const double start = Now();
        const bool   runs  = RunUpdates({column, seat, copy ? 0 : seat, controller});
        search_pins::Place();  // the search pins through the roots' contexts (L1: the column's; hud/field/search_pins.h)
        {
            // The chocobo wrangle's targets project through the primary controller (hud/field/wrangle_targets.h): a
            // copy holds the mirror's for this call alone.
            std::optional<scopes::primary_controller::Scope> mirror;
            if (seat == 0 && Acts(kLeverPrimary))
            {
                mirror.emplace(playerOne, controller);
                if (const bool held = mirror->Held(); static_cast<int>(held) != g_mirrorPrimaryHeld)
                {
                    g_mirrorPrimaryHeld = static_cast<int>(held);
                    platform::Log(L"HUD levers: a mirror view's copy {}",
                                  held ? std::wstring(L"places the chocobo wrangle's targets from its own camera")
                                       : std::format(L"cannot hold the primary controller ({}): the chocobo wrangle's "
                                                     L"targets keep player 1's places there",
                                                     scopes::primary_controller::Describe(mirror->Why())));
                }
            }
            wrangle_targets::Place(seat);
        }
        if (!copy)
        {
            jet_gauges::Place(seat);  // the seat's chocobo's jet gauge from the seat's view (hud/field/jet_gauges.h)
        }
        Charge(kTicksSeat, start);
        if (!runs)
        {
            Refuse(seat == 0 ? L"a HUD update raised an exception in a mirror view's run"
                             : players::BySeat(seat, L"a HUD update raised an exception in player 2's run",
                                               L"a HUD update raised an exception in player 3's run"));
        }
        if (!g_loggedEnter[c])
        {
            g_loggedEnter[c] = true;
            platform::Log(L"HUD levers: {} in column {} with {} HUD root(s); the column's marker host set {}",
                          seat == 0 ? std::wstring(L"a copy of player 1's HUD")
                          : copy    ? std::format(L"{}'s pass with a copy of player 1's HUD", players::SeatName(seat))
                                    : std::format(L"{}'s pass", players::SeatName(seat)),
                          column + 1, hud_roots::Count(),
                          g_columnHostSetKnown[c] ? L"from the last frame" : L"not known yet (first frame)");
        }
        return true;
    }
    catch (...)
    {
        // Only the log lines allocate; whatever was switched before the throw is put back (no render ran: the
        // column's set is not read back).
        LeaveLevers(column, false, false);
        g_on.store(false, std::memory_order_relaxed);
        return false;
    }
}

}  // namespace

void* ColumnController() noexcept
{
    return g_columnController;
}

bool EnterSeat(int column, int seat, bool copy) noexcept
{
    const bool entered = EnterSeatLevers(column, seat, copy || seat == 0);
    if (!entered)
    {
        g_columnController = nullptr;
        SettleSeats();  // the pass renders player 1's content: an earlier pass's contexts and positions put back
    }
    return entered;
}

void LeaveSeat(int column, int seat, bool another) noexcept
{
    static_cast<void>(seat);  // the pass's seat: EnterSeat's pair (0 for a mirror's)
    LeaveLevers(column, another, true);
}

namespace {

void LeaveLevers(int column, bool another, bool rendered) noexcept
{
    // Every lever is put back whatever happens to the others: each step is guarded on its own
    // (the engine calls by SEH inside the helpers), and nothing here allocates except the log.
    // With another further pass to come, the roots' contexts and player 1's re-run wait for the
    // last pass: the next pass's own contexts and updates replace this one's, and the roots go
    // back to player 1 once (hud/context_owners.h).
    if (rendered && column >= 1 && column < overlay_pass::kMaxColumns)
    {
        const auto c            = static_cast<std::size_t>(column);
        g_columnHostSetRead[c]  = ReadHostSet(g_columnHostSet[c]);  // written by the column's render
        g_columnHostSetKnown[c] = g_columnHostSetRead[c];
    }
    g_firstPlayerAnswer = 0;
    g_columnController  = nullptr;
    double mark         = Now();
    bool   contexts     = true;
    if (!another)
    {
        contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(0);
        mark     = Charge(kContextsOne, mark);
    }
    g_primary.reset();  // SwapWindow's close: a no-op unless it opened
    PutBackPlayerOneHostSet();
    bool ticks = true;
    mark       = Now();
    if (!another)
    {
        ticks = RunUpdates({});     // player 1's positions again, before player 1's render
        search_pins::Place();       // and the search pins'
        wrangle_targets::Place(0);  // and the chocobo wrangle's targets'
    }
    else if (g_holderShown != 0)
    {
        // The minimap's holder back to player 1's: the next seat's run skips it while that seat's minimap is off
        // (just after a join), and its pass would paint this seat's map. A copy's run left player 1's already.
        minimap::RunHolder(0);
    }
    Charge(kTicksOne, mark);
    RestoreTimeStep();
    g_playerOneDue    = another;
    g_passedThisFrame = true;
    RefuseWayBack(contexts, ticks);
}

}  // namespace

void SettleSeats() noexcept
{
    if (!g_playerOneDue)
    {
        return;
    }
    g_playerOneDue = false;
    ++g_settledLate;
    PutBackPlayerOneHostSet();  // player 1's re-run reads player 1's set
    HoldTimeStep();  // L6 for player 1's re-run, as inside a pass
    double     mark     = Now();
    const bool contexts = !Acts(kLeverContexts) || hud_roots::SetContexts(0);
    mark                = Charge(kContextsOne, mark);
    const bool ticks    = RunUpdates({});
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

void RegisterSeatHudContentCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"hud-levers",
         .shape = control::Shape::OnOff,
         .help  = L"player 2's and 3's images show their own content: the game's player levers switched around "
                  L"their pass",
         .set   = [](const Value& v) {
             if (!SeatHudContentInstalled())
             {
                 return Outcome{false, L"the HUD levers are not installed (see the startup log)"};
             }
             const bool on = std::get<bool>(v);
             SetSeatHudContent(on);
             if (on && !SeatHudContentOn())
             {
                 return Outcome{false, L"the HUD levers refused a check (see the log)"};
             }
             return Outcome{true, !on                  ? L"players 2 and 3's content off"
                                  : ColumnHudImagesOn() ? L"players 2 and 3's content on"
                                                        : L"players 2 and 3's content on (needs hud-image on)"};
         },
         .get = [] { return Value{SeatHudContentOn()}; }});
    control::Register(
        {.name  = L"hud-levers-mask",
         .shape = control::Shape::Integer,
         .max   = 511,
         .help  = L"diagnosis: which levers act (1 contexts, 2 first player, 4 primary controller, 8 the marker host set, 16 "
                  L"updates, 32 compass, 64 the arrows' own timers, 128 the enemy gauges per view, 256 the battle "
                  L"popups per view)",
         .set   = [](const Value& v) {
             SetSeatLeversMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
             return Outcome{true, L"levers mask set (see the log)"};
         },
         .get = [] { return Value{std::int64_t{SeatLeversMask()}}; }});
    control::Register(
        {.name  = L"hud-context-skip",
         .shape = control::Shape::OnOff,
         .help  = L"the command menu and the party status stay on player 1's context in players 2 and 3's passes "
                  L"(they never read it); off switches them like the others, for a comparison in the census",
         .set   = [](const Value& v) {
             const bool on = std::get<bool>(v);
             hud_roots::SetSkipContextFree(on);
             platform::Log(L"HUD levers: the command menu and the party status {}",
                           on ? L"left on player 1's context" : L"switched to each player's context");
             return Outcome{true, on ? L"skip on" : L"skip off"};
         },
         .get = [] { return Value{hud_roots::SkipContextFree()}; }});
}

#endif

}  // namespace lss::hud
