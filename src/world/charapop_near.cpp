#include "world/charapop_near.h"

#include "facts/world.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "scopes/leader_scope.h"
#include "world/charapop_facts.h"
#include "world/charapop_rule.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::charapop_near {

namespace {

using Clock = std::chrono::steady_clock;

// CharaPop_SetEntryMinLODLevel_ByKey(const key*, u8 level, bool evaluateNow): through the game root's
// manager to CharaPopObj_SetEntryMinLODLevel_Impl, which writes entry+0x1A1 and, with evaluateNow, queues the
// key for the next LOD tick; off the game thread it posts itself as a game-thread task
// (RE-p2-charapop-per-view.md §1.5). Entry: mov eax,28h; call __alloca_probe; sub rsp,rax; mov rax,rcx.
constexpr std::uint8_t kSetMinLevelBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x32, 0x07,
                                              0x14, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xC1};
constexpr platform::FunctionFact kSetMinLevel{L"CharaPop_SetEntryMinLODLevel_ByKey", 0x140F6E7F4, kSetMinLevelBytes};
using SetMinLevelFn = std::uint8_t (*)(const std::uint64_t* key, std::uint8_t level, bool evaluateNow);

// The evaluator runs for these states only (§1.3).
constexpr std::int32_t kFirstEvaluatedState = 4;
constexpr std::int32_t kLastEvaluatedState  = 9;

constexpr std::uint32_t kEvery          = 10;    // ticks between passes
constexpr std::uint32_t kReleasePasses  = 6;     // a raised entry goes back after this many passes in a row
                                                 // unneeded (about a second; margins in charapop_rule.h)
constexpr float         kReducedReach   = 0.5f;  // Detail::Reduced: half the game's distances
constexpr int           kMaxRaised      = 128;
constexpr int           kSkipSamples    = 3;     // "left to player 1" examples logged per summary period
constexpr int           kLeftRemembered = 32;    // entries already logged as left to player 1 (not logged again)
constexpr auto          kSummaryEvery   = std::chrono::seconds(60);

struct Raised
{
    std::uint64_t key       = 0;
    const void*   entry     = nullptr;
    std::uint8_t  baseLevel = 0;   // the entry's level before it was raised: picks Light or Middle
    std::uint32_t outside   = 0;   // passes in a row no seat's view needed it
    bool          seen      = false;
};

/// One pass's decisions, for the summary.
struct PassCounts
{
    int needed  = 0;  // entries a seat's view keeps (raised or not)
    int covered = 0;  // ... of which player 1's camera already keeps, so they were left to the game
};

std::uintptr_t g_gameRoot = 0;  // set once at install
SetMinLevelFn  g_setMinLevel = nullptr;
bool           g_installed = false;
std::atomic<Detail> g_detail{Detail::Full};

// Game thread.
std::array<Raised, kMaxRaised> g_raised{};
int                            g_raisedCount = 0;
const void*                    g_pop         = nullptr;  // the pop object the records belong to
std::uint32_t                  g_ticks       = 0;
std::uint32_t                  g_raisedTotal = 0, g_takenBack = 0;
std::uint32_t                  g_outOfReachTotal = 0, g_coveredTotal = 0, g_goneTotal = 0;  // releases, by reason
platform::Every g_summary{kSummaryEvery};
int                            g_skipSamples  = 0;
std::array<const void*, kLeftRemembered> g_leftLogged{};  // ring; an entry leaves it when raised
std::uint32_t                  g_leftNext     = 0;
bool                           g_fullLogged   = false;

float ReachOf(Detail detail) noexcept
{
    return detail == Detail::Reduced ? kReducedReach : 1.0f;
}

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a Debug report (platform/log.h)
    }
    try
    {
        platform::WriteLine(L"[debug] charapop near: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

float FloatAt(const std::byte* manager, std::uint32_t offset) noexcept
{
    return ReadField<float>(manager, offset);
}

/// The distances the evaluator uses for an entry of `level`, the manager's overrides applied (§1.3).
charapop::LodDistances DistancesFor(const std::byte* manager, std::uint8_t level) noexcept
{
    using namespace charapop;
    const bool middle = level == 2;
    LodDistances lod;
    lod.fovDegrees = FloatAt(manager, kShouldTickFov);
    lod.shouldTick = FloatAt(manager, middle ? kMiddleShouldTick : kLightShouldTick);
    lod.inCamera   = FloatAt(manager, middle ? kMiddleInCamera : kLightInCamera);
    lod.outCamera  = FloatAt(manager, middle ? kMiddleOutCamera : kLightOutCamera);
    lod.enableDraw = FloatAt(manager, middle ? kMiddleEnableDraw : kLightEnableDraw);
    lod.enemyAdded = FloatAt(manager, kEnemyInCameraAdded);
    if (ReadField<std::uint8_t>(manager, kOverrideInCamera + 4) != 0)
    {
        lod.inCamera = FloatAt(manager, kOverrideInCamera);
    }
    if (ReadField<std::uint8_t>(manager, kOverrideOutCamera + 4) != 0)
    {
        lod.outCamera = FloatAt(manager, kOverrideOutCamera);
    }
    if (ReadField<std::uint8_t>(manager, kOverrideEnableDraw + 4) != 0)
    {
        lod.enableDraw = FloatAt(manager, kOverrideEnableDraw);
    }
    return lod;
}

bool SetMinLevel(std::uint64_t key, std::uint8_t level) noexcept
{
    std::uint8_t found = 0;
    return platform::CallGuarded(g_setMinLevel, found, static_cast<const std::uint64_t*>(&key), level, true) && found != 0;
}

/// The record still names a live entry the mod raised: the entry reads the same key and still holds 3.
bool StillRaised(const Raised& raised) noexcept
{
    return ReadField<std::uint64_t>(raised.entry, charapop::kEntryKey) == raised.key &&
           ReadField<std::uint8_t>(raised.entry, charapop::kEntryMinLevel) == charapop::kLevelHeavy;
}

void Drop(int index) noexcept
{
    g_raised[static_cast<std::size_t>(index)] = g_raised[static_cast<std::size_t>(g_raisedCount - 1)];
    g_raised[static_cast<std::size_t>(g_raisedCount - 1)] = {};
    --g_raisedCount;
}

/// Clears `index` (only if it is still the mod's 3) and drops the record. True if it was cleared.
bool Clear(int index) noexcept
{
    const Raised& raised  = g_raised[static_cast<std::size_t>(index)];
    const bool    cleared = StillRaised(raised);
    if (cleared)
    {
        SetMinLevel(raised.key, 0);
    }
    else
    {
        ++g_takenBack;
    }
    Drop(index);
    return cleared;
}

/// Clears every record, or only drops them when the map's CharaPop is no longer the one they belong to.
void ClearAll(const wchar_t* why) noexcept
{
    if (g_raisedCount == 0)
    {
        return;
    }
    const int  count   = g_raisedCount;
    const bool samePop = charapop::ObjectsOf(g_gameRoot).pop == g_pop;
    while (g_raisedCount > 0)
    {
        if (samePop)
        {
            Clear(g_raisedCount - 1);
        }
        else
        {
            Drop(g_raisedCount - 1);
        }
    }
    // A real state change (switched off, the split closed, a new map, unloading): Info.
    platform::Log(L"charapop near: {} {} raised entr{} ({})", samePop ? L"cleared" : L"forgot (a new map)", count, count == 1 ? L"y" : L"ies", why);
}

int Find(const void* entry) noexcept
{
    for (int i = 0; i < g_raisedCount; ++i)
    {
        if (g_raised[static_cast<std::size_t>(i)].entry == entry)
        {
            return i;
        }
    }
    return -1;
}

/// The seats whose camera was read this pass, by seat (players/seats.h; index 0, player 1's, unused).
struct SeatCameras
{
    charapop::Camera camera[players::kMaxPlayers];
    std::uint8_t     read = 0;  // bit s: seat s's camera was read (charapop::SeatBit)

    bool Read(int seat) const noexcept { return (read & charapop::SeatBit(seat)) != 0; }
};

void LogSummary(const charapop::Camera& one, const SeatCameras& seats, const PassCounts& counts, Detail detail)
{
    // Once a minute: each raise and clear has its own line, and busy scenes change the count each pass.
    const auto now = Clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    g_skipSamples = 0;
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    // Player 1's camera to each seat's, in seat order ("/" between seats).
    std::wstring apart;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        apart += std::format(L"{}{:.0f}", seat == 1 ? L"" : L"/",
                             one.read && seats.Read(seat)
                                 ? charapop::Distance(one.location, seats.camera[seat].location) / 100.0f
                                 : -1.0f);
    }
    Line(L"{}: {} raised now (cameras {} m apart); this pass {} view needed {}, player 1's camera already keeps {} of "
         L"them; so far {} raised, {} cleared ({} out of reach, {} kept by player 1, {} left the evaluated states), {} "
         L"taken over by the game",
         DetailName(detail), g_raisedCount, apart,
         players::kSeatsBuilt == 1 ? L"player 2's" : L"the further players'", counts.needed, counts.covered,
         g_raisedTotal, g_outOfReachTotal + g_coveredTotal + g_goneTotal, g_outOfReachTotal, g_coveredTotal, g_goneTotal,
         g_takenBack);
}

/// "{d} m at {a} deg" from one camera, or "unread".
std::wstring Where(const charapop::EntryView* view) noexcept
{
    try
    {
        return view != nullptr ? std::format(L"{:.0f} m at {:.0f} deg", view->distance / 100.0f, view->angleDegrees)
                               : std::wstring(L"unread");
    }
    catch (...)
    {
        return {};
    }
}

/// One evaluated entry as player 1's camera and each read seat's camera see it.
struct Sighting
{
    const std::byte*       entry = nullptr;
    void*                  actor = nullptr;
    std::uint8_t           level = 0;
    charapop::EntryView    seat[players::kMaxPlayers];  // by seat; only the read seats' (SeatCameras::read)
    std::uint8_t           read = 0;                    // the seats whose view is in `seat`
    charapop::EntryView    one;
    const charapop::EntryView* fromOne = nullptr;  // &one when player 1's camera was read
    charapop::LodDistances lod;

    bool Read(int s) const noexcept { return (read & charapop::SeatBit(s)) != 0; }
};

/// "player 2: {where}" for each read seat, in seat order ("; " between seats).
std::wstring SeatsWhere(const Sighting& s) noexcept
{
    try
    {
        std::wstring text;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (s.Read(seat))
            {
                text += std::format(L"{}{}: {}", text.empty() ? L"" : L"; ", players::SeatName(seat), Where(&s.seat[seat]));
            }
        }
        return text;
    }
    catch (...)
    {
        return {};
    }
}

/// An entry the mod raised: kept while any read seat's view needs it, or released after kReleasePasses passes no
/// seat's view needed it (counted by reason).
void HandleRaised(int index, const Sighting& s, float reach, PassCounts& counts)
{
    using namespace charapop;
    Raised& raised = g_raised[static_cast<std::size_t>(index)];
    raised.seen    = true;
    if (ReadField<std::uint8_t>(s.entry, kEntryMinLevel) != kLevelHeavy)
    {
        ++g_takenBack;  // the game changed the minimum: it is the game's now
        Drop(index);
        return;
    }
    SeatHolds holds;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (s.Read(seat))
        {
            holds.Add(seat, HoldOf(s.lod, s.seat[seat], s.fromOne, reach));
        }
    }
    const Hold hold = holds.Combined();
    if (hold != Hold::OutOfReach)
    {
        ++counts.needed;
        counts.covered += hold == Hold::PlayerOneCovers ? 1 : 0;
    }
    raised.outside = hold == Hold::Keep ? 0 : raised.outside + 1;
    if (raised.outside < kReleasePasses)
    {
        return;
    }
    const bool covered = hold == Hold::PlayerOneCovers;
    if (platform::Enabled(platform::Level::Debug))
    {
        Line(L"cleared {} ({}; {}, player 1: {})", NameOf(s.actor),
             covered ? L"player 1's camera keeps it"
                     : (players::kSeatsBuilt == 1 ? L"out of player 2's reach" : L"out of every further player's reach"),
             SeatsWhere(s), Where(s.fromOne));
    }
    if (Clear(index))
    {
        ++(covered ? g_coveredTotal : g_outOfReachTotal);
    }
}

/// An entry not raised: raised when a read seat's view needs it and player 1's camera does not keep it.
void HandleCandidate(const Sighting& s, float reach, Detail detail, PassCounts& counts)
{
    using namespace charapop;
    std::uint8_t needing = 0;  // the seats whose view keeps it
    std::uint8_t raising = 0;  // ... and player 1's camera does not
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (s.Read(seat) && KeepsAwakeOrShown(s.lod, s.seat[seat], reach))
        {
            needing |= SeatBit(seat);
            raising |= ShouldRaise(s.lod, s.seat[seat], s.fromOne, reach) ? SeatBit(seat) : std::uint8_t{0};
        }
    }
    if (needing == 0)
    {
        return;
    }
    ++counts.needed;
    const auto logged = std::find(g_leftLogged.begin(), g_leftLogged.end(), s.entry);
    if (raising == 0)
    {
        ++counts.covered;
        // Each entry once (the same few were logged several times a second, live 2026-10-03).
        if (platform::Enabled(platform::Level::Debug) && logged == g_leftLogged.end() && g_skipSamples < kSkipSamples)
        {
            ++g_skipSamples;
            g_leftLogged[g_leftNext++ % g_leftLogged.size()] = s.entry;
            Line(L"left to player 1's camera: {} (level {}; {}, player 1: {})", NameOf(s.actor), s.level, SeatsWhere(s),
                 Where(s.fromOne));
        }
        return;
    }
    if (logged != g_leftLogged.end())
    {
        *logged = nullptr;  // raised now: a later "left" is a change again
    }
    if (g_raisedCount == kMaxRaised)
    {
        if (!g_fullLogged)
        {
            g_fullLogged = true;
            platform::Log(L"charapop near: {} entries raised, the most kept: further entries near {} are left as the "
                          L"game has them",
                          kMaxRaised, players::kSeatsBuilt == 1 ? L"player 2" : L"the further players");
        }
        return;
    }
    const std::uint64_t key = ReadField<std::uint64_t>(s.entry, kEntryKey);
    if (key == 0 || !SetMinLevel(key, kLevelHeavy))
    {
        return;
    }
    g_raised[static_cast<std::size_t>(g_raisedCount++)] = {key, s.entry, s.level, 0, true};
    ++g_raisedTotal;
    if (platform::Enabled(platform::Level::Debug))
    {
        Line(L"raised {} ({}, level {}; {}, player 1: {}, which does not keep it)", NameOf(s.actor), DetailName(detail),
             s.level, SeatsWhere(s), Where(s.fromOne));
    }
}

void Pass(Detail detail, const std::array<bool, players::kMaxPlayers>& playing)
{
    using namespace charapop;
    const Objects objects = ObjectsOf(g_gameRoot);
    if (objects.pop != g_pop)
    {
        // A new map's CharaPop (or none): the old records name entries that no longer exist.
        for (int i = 0; i < g_raisedCount; ++i)
        {
            g_raised[static_cast<std::size_t>(i)] = {};
        }
        g_raisedCount = 0;
        g_pop         = objects.pop;
    }
    if (objects.pop == nullptr)
    {
        return;
    }
    const Camera one = CameraOf(players::generation::PlayerOneController());
    SeatCameras  seats;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (playing[seat])
        {
            seats.camera[seat] = CameraOf(players::generation::ControllerOf(seat));
            seats.read |= seats.camera[seat].read ? SeatBit(seat) : std::uint8_t{0};
        }
    }
    if (seats.read == 0)
    {
        return;
    }
    for (int i = 0; i < g_raisedCount; ++i)
    {
        g_raised[static_cast<std::size_t>(i)].seen = false;
    }

    const float      reach = ReachOf(detail);
    PassCounts       counts;
    std::int32_t     count = 0;
    const std::byte* list  = EntryList(objects.pop, count);
    for (std::int32_t n = 0; n < count; ++n)
    {
        Sighting s;
        s.entry = static_cast<const std::byte*>(ReadPointer(list, static_cast<std::uint32_t>(n) * sizeof(void*)));
        if (s.entry == nullptr)
        {
            continue;
        }
        const std::int32_t state = ReadField<std::int32_t>(s.entry, kEntryState);
        if (state < kFirstEvaluatedState || state > kLastEvaluatedState)
        {
            continue;
        }
        const int  index    = Find(s.entry);
        const auto minLevel = ReadField<std::uint8_t>(s.entry, kEntryMinLevel);
        s.level             = ReadField<std::uint8_t>(s.entry, kEntryLevel);
        if (index < 0 && (minLevel != 0 || s.level >= kLevelHeavy || s.level == 0))
        {
            continue;  // the game set a minimum, or already treats it as Heavy
        }
        s.actor = scopes::leader_scope::ObjectAt(s.entry + kEntryActor);
        float at[3]{};
        if (s.actor == nullptr || !ActorLocation(s.actor, at))
        {
            continue;
        }
        const auto category = ReadField<std::uint8_t>(s.entry, kEntryCategory);
        const bool hides    = category == 1;
        const bool enemy    = category == 4;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (seats.Read(seat))
            {
                const Camera& camera = seats.camera[seat];
                s.seat[seat]         = {Distance(at, camera.location), AngleOff(camera, at), hides, enemy};
            }
        }
        s.read = seats.read;
        if (one.read)
        {
            s.one     = {Distance(at, one.location), AngleOff(one, at), hides, enemy};
            s.fromOne = &s.one;
        }
        // Light or Middle by the level the entry had before the mod raised it.
        s.lod = DistancesFor(objects.manager, index >= 0 ? g_raised[static_cast<std::size_t>(index)].baseLevel : s.level);
        if (index >= 0)
        {
            HandleRaised(index, s, reach, counts);
        }
        else
        {
            HandleCandidate(s, reach, detail, counts);
        }
    }
    // Records whose entry left the evaluated states (or the list): cleared if still the mod's.
    for (int i = g_raisedCount - 1; i >= 0; --i)
    {
        if (!g_raised[static_cast<std::size_t>(i)].seen && Clear(i))
        {
            ++g_goneTotal;
        }
    }
    LogSummary(one, seats, counts, detail);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root   = image.Rebase(facts::kEndGameRoot, sizeof(void*));
    const auto setter = platform::FindFunction(image, kSetMinLevel);
    g_gameRoot        = root ? *root : 0;
    g_setMinLevel     = setter ? reinterpret_cast<SetMinLevelFn>(*setter) : nullptr;
    g_installed       = scopes::leader_scope::Installed() && g_gameRoot != 0 && g_setMinLevel != nullptr;
    platform::Log(L"charapop near: {}", g_installed ? L"installed (characters near player 2 keep the detail player 2's camera "
                                                      L"gives them, through the game's minimum level; logged)"
                                                    : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Tick(bool splitShown, const std::array<bool, players::kMaxPlayers>& plays) noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    const Detail detail = g_detail.load(std::memory_order_relaxed);
    if (detail == Detail::Off)
    {
        ClearAll(L"switched off");
        return;
    }
    std::array<bool, players::kMaxPlayers> playing{};  // by seat: plays its member, its generation active
    bool                                   anyPlays = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        playing[seat] = plays[seat] && players::generation::Active(seat);
        anyPlays      = anyPlays || playing[seat];
    }
    if (!splitShown || !anyPlays)
    {
        ClearAll(players::kSeatsBuilt == 1 ? L"the split closed or player 2 stopped playing"
                                           : L"the split closed or no further player plays");
        return;
    }
    if (++g_ticks % kEvery != 0)
    {
        return;
    }
    try
    {
        Pass(detail, playing);
    }
    catch (...)
    {
    }
}

void SetDetail(Detail detail) noexcept
{
    g_detail.store(detail, std::memory_order_relaxed);
    const wchar_t* what = detail == Detail::Full    ? L"keep the detail player 2's camera gives them, where player 1's does not"
                          : detail == Detail::Reduced ? L"keep it within half the game's distances from player 2's camera"
                                                      : L"get only what player 1's camera gives them";
    platform::Log(L"charapop near: detail {} (characters near player 2 {})", DetailName(detail), what);
}

Detail CurrentDetail() noexcept
{
    return g_detail.load(std::memory_order_relaxed);
}

Detail NextDetail(Detail detail) noexcept
{
    return detail == Detail::Full ? Detail::Reduced : detail == Detail::Reduced ? Detail::Off : Detail::Full;
}

const wchar_t* DetailName(Detail detail) noexcept
{
    return detail == Detail::Full ? L"full" : detail == Detail::Reduced ? L"reduced" : L"off";
}

bool Installed() noexcept
{
    return g_installed;
}

void Release() noexcept
{
    // Any thread: the setter posts itself to the game thread when called off it (§1.5), and the tick that
    // shares these records is unregistered first (~LocalSplitScreenMod in mod.cpp unregisters the engine tick
    // callback before calling this; the records are not locked, so keep that order).
    if (g_installed && g_pop != nullptr)
    {
        ClearAll(L"the mod is unloading");
    }
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"npc-detail",
                       .shape = control::Shape::Word,
                       .words = {L"full", L"reduced", L"off", L"on"},
                       .help  = L"characters near player 2 that player 1's camera does not keep get full detail "
                                L"within the game's distances from player 2's camera (full, at start; on means "
                                L"full), within half of them (reduced), or only what player 1's camera gives them "
                                L"(off)",
                       .set   = [](const Value& v) {
                           if (!Installed())
                           {
                               return Outcome{false, L"the NPC detail lever is not installed (see the startup "
                                                     L"log)"};
                           }
                           const std::wstring& word   = std::get<std::wstring>(v);
                           const Detail        detail = word == L"reduced" ? Detail::Reduced
                                                        : word == L"off"   ? Detail::Off
                                                                           : Detail::Full;
                           SetDetail(detail);  // logs the new state
                           return Outcome{true, detail == Detail::Full
                                                    ? L"characters near player 2 keep full detail where player 1's "
                                                      L"camera does not (the default)"
                                                : detail == Detail::Reduced
                                                    ? L"characters near player 2 keep full detail within half the "
                                                      L"game's distances"
                                                    : L"characters near player 2 get only what player 1's camera "
                                                      L"gives them"};
                       },
                       .get = [] {
                           return Value{std::wstring(Installed() ? DetailName(CurrentDetail()) : L"off")};
                       }});
}

#endif

}  // namespace lss::world::charapop_near
