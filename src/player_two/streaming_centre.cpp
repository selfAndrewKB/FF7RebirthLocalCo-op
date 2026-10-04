#include "player_two/streaming_centre.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_centre.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace lss::player_two::streaming_centre {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-world-streaming.md §9 (this build).
constexpr std::uintptr_t kManagerGlobal = 0x14939BC98;  // the streaming manager (a pointer global)
constexpr std::uintptr_t kGridCall      = 0x140A9EE49;  // EndStreamingMgr_Tick's call of the grid update (rcx mgr, rdx &point)
constexpr std::uintptr_t kGridUpdate    = 0x140A9F900;  // EndStreamingMgr_UpdateGridCells_ByPoint
constexpr std::uintptr_t kBuildBoxes    = 0x1416F6CEC;  // EndStreamingGrid_BuildCellLoadBoxes(grid, const FVector* offset)
constexpr std::uint8_t   kBuildBoxesBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8};  // mov eax, 38h; call __alloca_probe
using GridFn  = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
// The grid update's cell load and release calls (logged at Debug): EndLevelLoader_RequestLoad(loader, 0x80, FName level, flag,
// const FVector* offset, 0), six arguments, two on the stack; EndLevel_ReleaseRequest(0xFF, FName level, 0).
constexpr std::uintptr_t kLoadCall    = 0x140A9FBF9;
constexpr std::uintptr_t kLoad        = 0x141158930;
constexpr std::uintptr_t kReleaseCall = 0x140A9FABA;
constexpr std::uintptr_t kRelease     = 0x141158890;
// CharaPop's area data hears each sub-level event (§9.1): CharaPopAreaData_Vt20_OnLevelEvent(areaData, FName level, mode);
// mode 1 clears that level's slots. Logged at Debug: whether a chest's area registers as player 2 comes near.
constexpr std::uintptr_t kAreaEvent        = 0x140FF7998;
constexpr std::uint8_t   kAreaEventBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10};  // mov [rsp+10h], rdx
using LoadFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using BuildFn = void (*)(void* grid, const void* offset);

constexpr std::uint32_t kOverride   = 0x3A4;  // mgr: FVector, the game's own override point (zero: none)
constexpr std::uint32_t kGrids      = 0x248;  // mgr: AEndStreamingGrid* array
constexpr std::uint32_t kGridCount  = 0x250;  // mgr: i32
constexpr std::uint32_t kCells      = 0x240;  // grid: cell array (0x78 bytes each)
constexpr std::uint32_t kCellCount  = 0x248;  // grid: i32
constexpr std::uint32_t kHeightCut  = 0x238;  // grid: float ThresholdCheckedHeightCollisionStream (read live)
constexpr std::uint32_t kGridOffset = 0x25C;  // grid: FVector, the level offset the boxes are built with
constexpr std::uint32_t kCellStride = 0x78;
constexpr std::uint32_t kCellLoad   = 0x28;   // cell: float LoadDistance (cooked; built into the box)
constexpr std::int32_t  kMaxGrids   = 256;
constexpr std::int32_t  kMaxCells   = 16384;
constexpr float         kStepCm     = 500.0f;  // the widening moves in 5 m steps (each step rebuilds the boxes)
constexpr std::uint32_t kEvery      = 10;      // ticks between passes
constexpr auto          kSummaryEvery = std::chrono::seconds(60);

// Set once at install.
platform::CallRedirect g_gridCall, g_loadCall, g_releaseCall;
platform::EntryRedirect g_areaEvent;
std::uintptr_t         g_managerGlobal = 0;
BuildFn                g_buildBoxes    = nullptr;
bool                   g_installed     = false;

// Game thread.
/// A cooked value the mod widened: the game's value and what the mod last wrote.
struct Widened
{
    float original = 0.0f;
    float written  = 0.0f;
};
std::unordered_map<const void*, Widened> g_cells;    // by cell
std::unordered_map<const void*, Widened> g_heights;  // by grid
std::unordered_map<const void*, Widened> g_nextCells, g_nextHeights;  // the pass's scratch (kept: its buckets are reused)
std::atomic<bool> g_on{false};           // the grid update gets the midpoint (Release may clear it off the game thread)
bool              g_broken     = false;  // an allocation failed: the feature stays off
bool              g_deferring  = false;  // the game's own override is set
float             g_point[4]   = {};     // the midpoint (an FVector, padded)
float             g_widenedBy  = -1.0f;  // the step applied, cm (-1: none)
std::uint32_t     g_ticks      = 0;
std::uint32_t     g_rebuilds   = 0;      // grids rebuilt since the last summary
Clock::time_point g_lastSummary{};

const std::byte* Manager() noexcept
{
    return static_cast<const std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_managerGlobal), 0));
}

bool OverrideSet(const std::byte* manager) noexcept
{
    float point[3]{};
    return platform::CopyGuarded(point, manager + kOverride, sizeof(point)) &&
           (point[0] != 0.0f || point[1] != 0.0f || point[2] != 0.0f);
}

bool Write(void* address, float value) noexcept
{
    return platform::WriteGuarded(address, &value, sizeof(value));
}

/// Applies `by` (cm) to every live cell's LoadDistance and `height` to every grid's height threshold, from the game's
/// values; `by` < 0 puts the game's values back. Rebuilds each grid whose cells changed with the game's own call.
/// Returns the number of cells now widened.
std::size_t ApplyOrThrow(const std::byte* manager, float by, float height)
{
    auto& cells   = g_nextCells;
    auto& heights = g_nextHeights;
    cells.clear();
    heights.clear();
    const auto* grids = static_cast<const std::byte*>(ReadPointer(manager, kGrids));
    const auto  count = ReadField<std::int32_t>(manager, kGridCount);
    for (std::int32_t g = 0; grids != nullptr && g < count && g < kMaxGrids; ++g)
    {
        auto* grid = static_cast<std::byte*>(ReadPointer(grids, static_cast<std::uint32_t>(g * sizeof(void*))));
        if (grid == nullptr)
        {
            continue;
        }
        bool        changed   = false;
        auto*       cellArray = static_cast<std::byte*>(ReadPointer(grid, kCells));
        const auto  cellCount = ReadField<std::int32_t>(grid, kCellCount);
        for (std::int32_t c = 0; cellArray != nullptr && c < cellCount && c < kMaxCells; ++c)
        {
            std::byte* field = cellArray + static_cast<std::size_t>(c) * kCellStride + kCellLoad;
            float      now   = 0.0f;
            if (!platform::CopyGuarded(&now, field, sizeof(now)))
            {
                continue;
            }
            Widened    record = {now, now};
            const auto known  = g_cells.find(field);
            if (known != g_cells.end() && known->second.written == now)
            {
                record = known->second;  // still the mod's value
            }
            const float want = by < 0.0f ? record.original : record.original + by;
            if (want != now && Write(field, want))
            {
                changed = true;
            }
            record.written = want;
            if (by >= 0.0f)
            {
                cells[field] = record;
            }
        }
        std::byte* cut = grid + kHeightCut;
        float      now = 0.0f;
        if (platform::CopyGuarded(&now, cut, sizeof(now)))
        {
            Widened    record = {now, now};
            const auto known  = g_heights.find(grid);
            if (known != g_heights.end() && known->second.written == now)
            {
                record = known->second;
            }
            const float want = by < 0.0f ? record.original : record.original + height;
            if (want != now)
            {
                Write(cut, want);
            }
            record.written = want;
            if (by >= 0.0f)
            {
                heights[grid] = record;
            }
        }
        if (changed && platform::CallGuarded(g_buildBoxes, static_cast<void*>(grid), static_cast<const void*>(grid + kGridOffset)))
        {
            ++g_rebuilds;
        }
    }
    g_cells.swap(cells);
    g_heights.swap(heights);
    return g_cells.size();
}

/// ApplyOrThrow; false when an allocation failed (the feature then stops for the session, logged once).
bool Apply(const std::byte* manager, float by, float height, std::size_t& widened) noexcept
{
    try
    {
        widened = ApplyOrThrow(manager, by, height);
        return true;
    }
    catch (...)
    {
        if (!g_broken)
        {
            g_broken = true;
            g_on.store(false, std::memory_order_relaxed);
            platform::Log(L"streaming centre: WARNING: out of memory while widening; off for this session (the cells already "
                          L"widened are put back when player 2 stops playing)");
        }
        return false;
    }
}

void Stop(const wchar_t* why) noexcept
{
    const bool had = g_on.exchange(false, std::memory_order_relaxed);
    bool       restored = false;
    if (g_widenedBy >= 0.0f)
    {
        // Without the manager (a field load) the cells cannot be walked: the bookkeeping stays and the next tick tries again.
        std::size_t left = 0;
        if (const auto* manager = Manager(); manager != nullptr && Apply(manager, -1.0f, 0.0f, left))
        {
            g_cells.clear();
            g_heights.clear();
            g_widenedBy = -1.0f;
            restored    = true;
        }
    }
    if (had || restored)
    {
        platform::Log(L"streaming centre: back to the game's own (grid cells around the leader, their cooked distances): {}",
                      why);
    }
}

bool CharacterAt(const void* character, float (&out)[3]) noexcept
{
    return character != nullptr && charapop::ActorLocation(const_cast<void*>(character), out);
}

void Pass() noexcept
{
    const auto* manager = Manager();
    if (manager == nullptr)
    {
        return;
    }
    if (OverrideSet(manager))
    {
        if (!g_deferring)
        {
            g_deferring = true;
            Stop(L"an event set the game's streaming override point; left to it until it clears");
        }
        return;
    }
    if (g_deferring)
    {
        g_deferring = false;
        platform::Log(L"streaming centre: the game's override point cleared; following both players again");
    }
    float       one[3]{}, two[3]{};
    const void* playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    if (!CharacterAt(playerOne, one) || !CharacterAt(generation::Member(), two))
    {
        return;
    }
    for (int i = 0; i < 3; ++i)
    {
        g_point[i] = (one[i] + two[i]) * 0.5f;
    }
    const float apart  = charapop::Distance(one, two);
    const float by     = std::ceil(apart * 0.5f / kStepCm) * kStepCm;
    const float height = std::ceil(std::fabs(one[2] - two[2]) * 0.5f / kStepCm) * kStepCm;
    // Every pass: grids come and go with their levels, and a cell the game rewrote takes its new value as its own.
    std::size_t widened = 0;
    if (g_broken || !Apply(manager, by, height, widened))
    {
        return;
    }
    if (!g_on.load(std::memory_order_relaxed))
    {
        platform::Log(L"streaming centre: the grid cells load around the midpoint of both players, widened by half their "
                      L"separation ({:.0f} m now, {} cells)",
                      by / 100.0f, widened);
    }
    g_on.store(true, std::memory_order_relaxed);
    g_widenedBy = by;
    if (platform::Enabled(platform::Level::Debug) && Clock::now() - g_lastSummary >= kSummaryEvery)
    {
        g_lastSummary = Clock::now();
        platform::LogDebug(L"streaming centre: midpoint ({:.0f}, {:.0f}, {:.0f}), players {:.0f} m apart; {} cells widened by "
                           L"{:.0f} m, height threshold +{:.0f} m; {} grid rebuild(s) since the last line",
                           g_point[0], g_point[1], g_point[2], apart / 100.0f, widened, by / 100.0f, height / 100.0f,
                           g_rebuilds);
        g_rebuilds = 0;
    }
}

/// A grid cell's sub-level requested (Debug): its name, and whether the midpoint was the point.
std::uint64_t CellLoad(std::uint64_t loader, std::uint64_t kind, std::uint64_t level, std::uint64_t flag, std::uint64_t offset,
                   std::uint64_t last) noexcept
{
    if (platform::Enabled(platform::Level::Debug) && generation::OnGameThread())
    {
        platform::LogDebug(L"streaming centre: grid cell level {} requested ({})", players::unreal::NameText(level),
                           g_on.load(std::memory_order_relaxed) ? L"the midpoint, widened" : L"the game's point");
    }
    return reinterpret_cast<LoadFn>(g_loadCall.Original())(loader, kind, level, flag, offset, last);
}

/// A grid cell's sub-level released (Debug).
std::uint64_t CellRelease(std::uint64_t who, std::uint64_t level, std::uint64_t r8, std::uint64_t r9) noexcept
{
    if (platform::Enabled(platform::Level::Debug) && generation::OnGameThread())
    {
        platform::LogDebug(L"streaming centre: grid cell level {} released ({})", players::unreal::NameText(level),
                           g_on.load(std::memory_order_relaxed) ? L"the midpoint, widened" : L"the game's point");
    }
    return reinterpret_cast<GridFn>(g_releaseCall.Original())(who, level, r8, r9);
}

/// A sub-level event CharaPop's area data hears (Debug).
std::uint64_t AreaEvent(std::uint64_t areaData, std::uint64_t level, std::uint64_t mode, std::uint64_t r9) noexcept
{
    if (platform::Enabled(platform::Level::Debug) && generation::OnGameThread())
    {
        platform::LogDebug(L"streaming centre: CharaPop area data: level {} event, mode {}", players::unreal::NameText(level),
                           static_cast<unsigned>(mode & 0xFF));
    }
    return reinterpret_cast<GridFn>(g_areaEvent.Original())(areaData, level, mode, r9);
}

/// The streaming tick's grid update: the midpoint while the mod follows both players, else the game's point.
std::uint64_t GridUpdate(std::uint64_t manager, std::uint64_t point, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto call = reinterpret_cast<GridFn>(g_gridCall.Original());
    // The game's own override point (an event's) wins at once, not only at the next pass.
    if (g_on.load(std::memory_order_relaxed) && generation::OnGameThread() &&
        !OverrideSet(reinterpret_cast<const std::byte*>(manager)))
    {
        return call(manager, reinterpret_cast<std::uint64_t>(g_point), r8, r9);
    }
    return call(manager, point, r8, r9);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(kManagerGlobal, sizeof(void*));
    const auto build   = image.Rebase(kBuildBoxes);
    const auto site    = image.Rebase(kGridCall, 5);
    const auto update  = image.Rebase(kGridUpdate);
    g_managerGlobal    = manager ? *manager : 0;
    g_buildBoxes       = build && image.BytesMatch(kBuildBoxes, kBuildBoxesBytes) ? reinterpret_cast<BuildFn>(*build) : nullptr;
    g_installed        = g_managerGlobal != 0 && g_buildBoxes != nullptr && site && update &&
                  g_gridCall.Install(*site, *update, reinterpret_cast<std::uintptr_t>(&GridUpdate));
    const auto loadSite    = image.Rebase(kLoadCall, 5);
    const auto load        = image.Rebase(kLoad);
    const auto releaseSite = image.Rebase(kReleaseCall, 5);
    const auto release     = image.Rebase(kRelease);
    const bool loads = loadSite && load && g_loadCall.Install(*loadSite, *load, reinterpret_cast<std::uintptr_t>(&CellLoad));
    const bool releases =
        releaseSite && release && g_releaseCall.Install(*releaseSite, *release, reinterpret_cast<std::uintptr_t>(&CellRelease));
    const auto areaEvent = image.Rebase(kAreaEvent);
    const bool areas     = areaEvent && image.BytesMatch(kAreaEvent, kAreaEventBytes) &&
                       g_areaEvent.Install(*areaEvent, kAreaEventBytes, reinterpret_cast<std::uintptr_t>(&AreaEvent));
    if (!loads || !releases || !areas)
    {
        platform::Log(L"streaming centre: cell loads {}, releases {}, area-data events {} (logging only)",
                      loads ? L"logged" : L"NOT logged", releases ? L"logged" : L"NOT logged", areas ? L"logged" : L"NOT logged");
    }
    platform::Log(L"streaming centre: {}", g_installed ? L"installed (the grid level streaming follows both players while "
                                                         L"split, with the NPC centre's switch)"
                                                       : L"UNAVAILABLE (a check failed; streaming stays around player 1)");
    return g_installed;
}

void Tick(bool playerTwoPlays) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    const bool enabled = charapop_centre::Enabled();
    // Not the split on screen: a whole-screen menu or scene keeps player 2's surroundings loaded (unloading them for a shop
    // and loading them again after would churn the cells each time either player opens a menu).
    if (!playerTwoPlays || !generation::Active() || !enabled)
    {
        g_deferring = false;
        Stop(!enabled ? L"switched off" : L"player 2 stopped playing");
        return;
    }
    if (++g_ticks % kEvery == 0)
    {
        Pass();
    }
}

void Release() noexcept
{
    if (!g_installed)
    {
        return;
    }
    if (generation::OnGameThread())
    {
        Stop(L"the mod is unloading");
    }
    else if (g_on.exchange(false, std::memory_order_relaxed) || g_widenedBy >= 0.0f)
    {
        platform::Log(L"streaming centre: unloading off the game thread; the widened cells stay until their level reloads");
    }
}

}  // namespace lss::player_two::streaming_centre
