#include "rides/treasure_hint.h"

#include "facts/engine.h"
#include "facts/level_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "hud/passes/pass_opacity.h"
#include "input/member_movement.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"
#include "rides/scour_scent.h"
#include "scopes/leader_scope.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace lss::rides::treasure_hint {

namespace {

struct Vec
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

using StatusWidgetsFn      = void (*)(void* session, std::uint8_t nearPoint, void* unused, std::uint8_t trail,
                                      std::uint8_t quest, std::uint8_t treasure, std::uint8_t burrow);
using PlaceIconFn          = void (*)(void* session, void* icon, std::int32_t mode);
using RemoveFn             = void (*)(void* widget);
using VisibilityFn         = void (*)(void* widget, std::uint8_t visibility);
using RideBodyFn           = void* (*)(std::uint8_t seatedOnly);
using ObjectTestFn         = std::uint8_t (*)(const void* object);
using FieldPointFn         = const void* (*)(const void* name);
using FindRowFn            = void* (*)(void* registry, std::uint64_t name, void* scratch, std::uint8_t* flag);
using ClosestKeyFn         = float (*)(const void* spline, const Vec* world);
using LocationAtKeyFn      = Vec* (*)(const void* spline, Vec* out, float key, int space);
using ReparamEvalFn        = float (*)(const void* curve, float distance, const float* fallback);
using LocationAtDistanceFn = Vec* (*)(const void* spline, Vec* out, float distance, int space);
using SplineLengthFn       = float (*)(const void* spline);

struct Functions
{
    StatusWidgetsFn      statusWidgets      = nullptr;
    PlaceIconFn          placeIcon          = nullptr;
    RemoveFn             remove             = nullptr;
    RideBodyFn           rideBody           = nullptr;
    ObjectTestFn         isEndCharacter     = nullptr;
    ObjectTestFn         storyFlag          = nullptr;
    FieldPointFn         fieldPoint         = nullptr;
    FindRowFn            findRow            = nullptr;
    ClosestKeyFn         closestKey         = nullptr;
    LocationAtKeyFn      locationAtKey      = nullptr;
    ReparamEvalFn        reparamEval        = nullptr;
    LocationAtDistanceFn locationAtDistance = nullptr;
    SplineLengthFn       splineLength       = nullptr;
};
Functions              g_fn;
void*                  g_params = nullptr;  // the craft's parameter registry
platform::CallRedirect g_statusCall;
bool                   g_ready = false;

using players::weak_object::kNoWeak;
constexpr float         kFarAway     = 1.0e7f;                 // the nearest search's first best distance
constexpr int           kDriftFrames = 2;  // a disagreement held this long is reported (a trail bound mid-tick: one)
constexpr std::size_t   kNamesKept   = 256;
constexpr std::size_t   kPointsReserved = 32;  // above an area's points: one allocation per reading

/// A hint's flags, as the game passes them to UpdateStatusWidgets, and the nearest point they were read from (the
/// mod's reading only).
struct Flags
{
    bool          nearPoint = false, trail = false, quest = false, treasure = false, burrow = false;
    std::uint64_t point     = 0;
    std::int32_t  type = 0, done = 0;
    std::uint8_t  hint = 0;
};

bool SameFlags(const Flags& a, const Flags& b) noexcept
{
    return a.nearPoint == b.nearPoint && a.trail == b.trail && a.quest == b.quest && a.treasure == b.treasure &&
           a.burrow == b.burrow;
}

/// The icon UpdateStatusWidgets shows for these flags (0 none).
int IconOf(const Flags& flags) noexcept
{
    const bool found = flags.treasure || flags.burrow;
    const bool quest = flags.quest || flags.trail;
    return found && quest ? facts::kCraftIconBoth : found ? facts::kCraftIconTreasure : quest ? facts::kCraftIconQuest : 0;
}

const wchar_t* IconName(int icon) noexcept
{
    switch (icon)
    {
    case facts::kCraftIconTreasure:
        return L"the treasure icon (9)";
    case facts::kCraftIconQuest:
        return L"the quest icon (10)";
    case facts::kCraftIconBoth:
        return L"both icons (11)";
    default:
        return L"no icon";
    }
}

std::wstring FlagsText(const Flags& flags)
{
    return std::format(L"near {} trail {} quest {} treasure {} burrow {}", static_cast<int>(flags.nearPoint),
                       static_cast<int>(flags.trail), static_cast<int>(flags.quest), static_cast<int>(flags.treasure),
                       static_cast<int>(flags.burrow));
}

std::wstring PointText(const Flags& flags)
{
    return flags.point == 0 ? std::wstring(L"none")
                            : std::format(L"{} (type {}, done {}, hint {})", players::unreal::NameText(flags.point),
                                          flags.type, flags.done, static_cast<int>(flags.hint));
}

// ---- The icon sets ----

/// A rider's own icon: the weaks of the icon and its two arrows, and the icon's mode and animation state.
struct IconSet
{
    std::uint64_t icon = kNoWeak, arrowA = kNoWeak, arrowB = kNoWeak;
    std::int32_t  mode = 0;
    std::uint8_t  anim = 0;
};

IconSet ReadSet(const void* session) noexcept
{
    return {ReadField<std::uint64_t>(session, facts::kCraftStatusIcon), ReadField<std::uint64_t>(session, facts::kCraftArrowA),
            ReadField<std::uint64_t>(session, facts::kCraftArrowB), ReadField<std::int32_t>(session, facts::kCraftIconMode),
            ReadField<std::uint8_t>(session, facts::kCraftIconAnim)};
}

void WriteSet(void* session, const IconSet& set) noexcept
{
    WriteField(session, facts::kCraftStatusIcon, set.icon);
    WriteField(session, facts::kCraftArrowA, set.arrowA);
    WriteField(session, facts::kCraftArrowB, set.arrowB);
    WriteField(session, facts::kCraftIconMode, set.mode);
    WriteField(session, facts::kCraftIconAnim, set.anim);
}

bool Empty(const IconSet& set) noexcept
{
    return players::weak_object::ObjectOf(set.icon) == nullptr && players::weak_object::ObjectOf(set.arrowA) == nullptr &&
           players::weak_object::ObjectOf(set.arrowB) == nullptr;
}

/// For one game call: `set` in the session's place, the session read as idle (no dig, sniff, discover effect or trail
/// effect; no dug item's widget, so the game's "item shown" branch neither places nor removes the owner's). At the end
/// `set` takes what the game left and every field is put back.
class SwappedIn
{
public:
    SwappedIn(void* session, IconSet& set) noexcept
        : session_(session),
          set_(set),
          saved_(ReadSet(session)),
          itemGet_(ReadField<std::uint64_t>(session, facts::kCraftItemGet)),
          sniff_(ReadField<std::uint64_t>(session, facts::kCraftSniffAction)),
          discover_(ReadField<std::int32_t>(session, facts::kCraftDiscoverMode)),
          digging_(ReadField<std::uint8_t>(session, facts::kCraftDigging)),
          trailEffect_(ReadField<std::uint8_t>(session, facts::kCraftTrailEffect))
    {
        WriteSet(session, set);
        WriteField(session, facts::kCraftItemGet, kNoWeak);
        WriteField(session, facts::kCraftSniffAction, std::uint64_t{0});
        WriteField(session, facts::kCraftDiscoverMode, std::int32_t{0});
        WriteField(session, facts::kCraftDigging, std::uint8_t{0});
        WriteField(session, facts::kCraftTrailEffect, std::uint8_t{0});
    }

    ~SwappedIn()
    {
        set_ = ReadSet(session_);
        WriteSet(session_, saved_);
        WriteField(session_, facts::kCraftItemGet, itemGet_);
        WriteField(session_, facts::kCraftSniffAction, sniff_);
        WriteField(session_, facts::kCraftDiscoverMode, discover_);
        WriteField(session_, facts::kCraftDigging, digging_);
        WriteField(session_, facts::kCraftTrailEffect, trailEffect_);
    }

    SwappedIn(const SwappedIn&)            = delete;
    SwappedIn& operator=(const SwappedIn&) = delete;

private:
    void*         session_;
    IconSet&      set_;
    IconSet       saved_;
    std::uint64_t itemGet_, sniff_;
    std::int32_t  discover_;
    std::uint8_t  digging_, trailEffect_;
};

// Game thread.
std::array<IconSet, players::kMaxPlayers> g_sets{};   // each rider's own set (empty for the session's player)
std::array<Flags, players::kMaxPlayers>   g_flags{};  // each player's hint this frame (the game's for the tick's)
std::array<bool, players::kMaxPlayers>    g_placedSaid{};
int                                       g_setPlayer  = 0;  // whose set the session holds
int                                       g_tickSaid   = -1;
bool                                      g_gameCalled = false;  // the game's UpdateStatusWidgets call this tick
Flags                                     g_gameFlags;
Flags                                     g_gameSaid;
bool                                      g_gameSaidOnce = false;
int                                       g_driftFrames  = 0;
bool                                      g_faulted = false, g_refusedSaid = false, g_bodySaid = false;
std::uint64_t                             g_iconName = 0;  // the FName ICON_DISTANCE_FROM_LINE (0: not looked up yet)
hud::HiddenSet                            g_hidden;

using scour_scent::Session;

void* RideBody() noexcept
{
    void* body = nullptr;
    return platform::CallGuarded(g_fn.rideBody, body, std::uint8_t{1}) ? body : nullptr;
}

/// Runs `fn` in `player`'s answers: player 1's as the world tick has them; a seat's primary scope and leader answer.
template <class Fn>
bool AsPlayer(int player, Fn& fn) noexcept
{
    if (player == 0)
    {
        fn();
        return true;
    }
    void* member = players::generation::MemberOf(player);
    return member != nullptr &&
           input::member_movement::InPrimaryScope(
               player, member, [](void* context) { (*static_cast<Fn*>(context))(); }, &fn);
}

/// A set removed as the game removes its own: each widget hidden, taken off the overlay, its weak reset.
void RemoveSet(IconSet& set) noexcept
{
    for (const std::uint64_t weak : {set.icon, set.arrowA, set.arrowB})
    {
        void* widget = players::weak_object::ObjectOf(weak);
        if (widget == nullptr)
        {
            continue;
        }
        const auto hide = reinterpret_cast<VisibilityFn>(ReadPointerAt(VTableOf(widget) + facts::kWidgetVisibilitySlot));
        if (hide != nullptr)
        {
            (void)platform::CallGuarded(hide, widget, facts::kWidgetHidden);
        }
        (void)platform::CallGuarded(g_fn.remove, widget);
    }
    set = {};
}

// ---- The nearest-point test, as the craft tick makes it (research/RE-chocobo-treasure-hint-seats.md) ----

Vec LocationAt(const void* object, std::uint32_t offset) noexcept
{
    return {ReadField<float>(object, offset), ReadField<float>(object, offset + sizeof(float)),
            ReadField<float>(object, offset + 2 * sizeof(float))};
}

// Distances in the game's own arithmetic (squares fused into one sum, then the root), so a test on the edge of a range
// falls the same way.
float Distance3(const Vec& a, const Vec& b) noexcept
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(std::fma(dz, dz, std::fma(dy, dy, dx * dx)));
}

float Distance2(const Vec& a, const Vec& b) noexcept
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(std::fma(dy, dy, dx * dx));
}

bool Named(const void* object, std::uint32_t offset) noexcept
{
    return ReadField<std::uint64_t>(object, offset) != 0;
}

/// The story flag named at `offset` in `point` is set.
bool FlagSet(const void* point, std::uint32_t offset) noexcept
{
    std::uint8_t set = 0;
    return Named(point, offset) &&
           platform::CallGuarded(g_fn.storyFlag, set, static_cast<const void*>(static_cast<const std::uint8_t*>(point) + offset)) &&
           set != 0;
}

const void* FieldPoint(const void* name) noexcept
{
    const void* transform = nullptr;
    return platform::CallGuarded(g_fn.fieldPoint, transform, name) ? transform : nullptr;
}

/// The point's bound quest trail's spline component, or null.
const void* SplineOf(const void* point) noexcept
{
    const void* actor = players::weak_object::ObjectIn(point, facts::kPointSpline);
    return actor != nullptr ? ReadPointer(actor, facts::kQuestSplineComponent) : nullptr;
}

bool LocationAtKey(const void* spline, float key, Vec& out) noexcept
{
    Vec* written = nullptr;
    return platform::CallGuarded(g_fn.locationAtKey, written, spline, &out, key, facts::kSplineWorldSpace) && written != nullptr;
}

bool LocationAtDistance(const void* spline, float distance, Vec& out) noexcept
{
    Vec* written = nullptr;
    return platform::CallGuarded(g_fn.locationAtDistance, written, spline, &out, distance, facts::kSplineWorldSpace) &&
           written != nullptr;
}

/// The point's path points: the array and its count.
std::pair<const std::uint8_t*, std::int32_t> PathOf(const void* point) noexcept
{
    return {static_cast<const std::uint8_t*>(ReadPointer(point, facts::kPointPath)),
            ReadField<std::int32_t>(point, facts::kPointPathCount)};
}

Vec PathLocation(const std::uint8_t* path, std::int32_t index) noexcept
{
    return LocationAt(path + static_cast<std::ptrdiff_t>(index) * facts::kPointPathElement, 0);
}

/// Whether a name's text holds "Quest" and "Craft" (ASCII, any case, as the game's FString::Find), cached by the name.
struct NameTests
{
    std::uint64_t name  = 0;
    bool          quest = false, craft = false;
};
std::vector<NameTests> g_names;

bool Holds(const std::wstring& text, const wchar_t* part) noexcept
{
    const auto        fold   = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
    const std::size_t length = std::char_traits<wchar_t>::length(part);
    for (std::size_t at = 0; at + length <= text.size(); ++at)
    {
        std::size_t i = 0;
        while (i < length && fold(text[at + i]) == fold(part[i]))
        {
            ++i;
        }
        if (i == length)
        {
            return true;
        }
    }
    return false;
}

NameTests TestsOf(std::uint64_t name) noexcept
{
    for (const NameTests& tests : g_names)
    {
        if (tests.name == name)
        {
            return tests;
        }
    }
    try
    {
        const std::wstring text = players::unreal::NameText(name);
        const NameTests    tests{name, Holds(text, L"quest"), Holds(text, L"craft")};
        if (text == L"?")
        {
            return tests;  // the name could not be read this time: tried again next time
        }
        if (g_names.size() >= kNamesKept)
        {
            g_names.clear();
        }
        g_names.push_back(tests);
        return tests;
    }
    catch (...)
    {
        return {name};
    }
}

/// The search's state: the best point so far and every point measured, in order (each once).
struct Search
{
    const void*              best     = nullptr;
    float                    distance = kFarAway;
    std::vector<const void*> measured;
};

void Consider(Search& search, const void* point, float distance) noexcept
{
    // A nearer point wins, but a point with no field point never replaces one that has one; ties keep the earlier.
    if (search.best == nullptr ||
        (!(distance >= search.distance) && (!Named(search.best, facts::kPointFieldPoint) || Named(point, facts::kPointFieldPoint))))
    {
        search.best     = point;
        search.distance = distance;
    }
}

void Measured(Search& search, const void* point)
{
    for (const void* each : search.measured)
    {
        if (each == point)
        {
            return;
        }
    }
    search.measured.push_back(point);
}

/// Every point measured from `at`: its field point when loaded, and its bound trail's start, path points and end.
/// `skip` is left out: the nearest point whose completion flag is set, which the game completes when it is the tick's
/// player's nearest and which leaves the table the next frame.
void Measure(const void* manager, const Vec& at, bool locked, const void* skip, Search& search)
{
    const auto table = reinterpret_cast<std::uintptr_t>(manager) + facts::kCraftPoints;
    platform::ForEachSparseElement(table, facts::kCraftPointElement, [&](std::uintptr_t element) {
        const void* point = ReadPointer(reinterpret_cast<const void*>(element), facts::kCraftPointObject);
        if (point == nullptr || point == skip)
        {
            return true;
        }
        const bool counts = !locked || Named(point, facts::kPointFieldPoint);
        if (const void* field = FieldPoint(reinterpret_cast<const void*>(element)); field != nullptr)
        {
            if (!counts)
            {
                return true;
            }
            Measured(search, point);
            Consider(search, point, Distance3(LocationAt(field, facts::kFieldPointLocation), at));
        }
        const void* spline = SplineOf(point);
        if (spline == nullptr || !counts)
        {
            return true;
        }
        Measured(search, point);
        Vec end;
        if (LocationAtDistance(spline, 0.0f, end))
        {
            Consider(search, point, Distance3(end, at));
        }
        const auto [path, count] = PathOf(point);
        for (std::int32_t i = 0; path != nullptr && i < count; ++i)
        {
            Consider(search, point, Distance3(PathLocation(path, i), at));
        }
        float length = 0.0f;
        if (platform::CallGuarded(g_fn.splineLength, length, spline) && LocationAtDistance(spline, length, end))
        {
            Consider(search, point, Distance3(end, at));
        }
        return true;
    });
}

/// The nearest point's flags (the 3D range sets "near", the 2D range its kind; a point marked done shows nothing).
void Classify(const void* best, const Vec& at, bool locked, Flags& flags)
{
    const bool kept = locked ? Named(best, facts::kPointFieldPoint) : ReadField<std::uint8_t>(best, facts::kPointHint) != 0;
    if (!kept)
    {
        return;
    }
    Vec target;
    if (Named(best, facts::kPointFieldPoint))
    {
        const void* field = FieldPoint(static_cast<const std::uint8_t*>(best) + facts::kPointKey);
        if (field == nullptr)
        {
            return;
        }
        target = LocationAt(field, facts::kFieldPointLocation);
    }
    else
    {
        const auto [path, count] = PathOf(best);
        if (path == nullptr || count <= 0)
        {
            return;
        }
        target = PathLocation(path, count - 1);
    }
    const float range = ReadField<float>(best, facts::kPointRange);
    if (Distance3(target, at) > range)
    {
        return;
    }
    flags.nearPoint = true;
    if (!(Distance2(target, at) > range))
    {
        const std::int32_t type = ReadField<std::int32_t>(best, facts::kPointType);
        if (TestsOf(ReadField<std::uint64_t>(best, facts::kPointActivation)).quest)
        {
            flags.quest = true;
        }
        else if (type == 1)
        {
            flags.burrow = true;
        }
        else if (type == 0 && TestsOf(ReadField<std::uint64_t>(best, facts::kPointKey)).craft)
        {
            flags.treasure = true;
        }
    }
    if (ReadField<std::int32_t>(best, facts::kPointDone) == 1)
    {
        flags = {};
    }
}

/// The reparam table's distance at segment `segment` (0 outside the spline's segments).
float SegmentDistance(const void* spline, int segment) noexcept
{
    const std::int32_t count    = ReadField<std::int32_t>(spline, facts::kSplinePointCount);
    const std::int32_t segments = ReadField<std::uint8_t>(spline, facts::kSplineClosedLoop) != 0 ? count : count - 1;
    if (segment < 0 || segment >= segments + 1)
    {
        return 0.0f;
    }
    const std::int32_t step = segment * ReadField<std::int32_t>(spline, facts::kSplineReparamSteps);
    return ReadField<float>(ReadPointer(spline, facts::kSplineReparam),
                            static_cast<std::uint32_t>(step) * facts::kSplineReparamElement);
}

/// A bound trail's hint: the rider near the spline, and short of its revealed part's end or near that end.
bool NearTrail(const void* spline, const void* point, const Vec& at, float iconDistance) noexcept
{
    float key = 0.0f;
    Vec   closest;
    if (!platform::CallGuarded(g_fn.closestKey, key, spline, &at) || !LocationAtKey(spline, key, closest) ||
        Distance2(closest, at) > iconDistance)
    {
        return false;
    }
    if (!platform::CallGuarded(g_fn.closestKey, key, spline, static_cast<const Vec*>(&closest)))
    {
        return false;
    }
    const int   segment  = static_cast<int>(key);
    const float from     = SegmentDistance(spline, segment);
    const float along    = std::fma(SegmentDistance(spline, segment + 1) - from, key - static_cast<float>(segment), from);
    const float revealed = ReadField<float>(point, facts::kPointRevealed);
    const float zero     = 0.0f;
    float       endKey   = 0.0f;
    Vec         end;
    const void* reparam = static_cast<const std::uint8_t*>(spline) + facts::kSplineReparam;
    if (!platform::CallGuarded(g_fn.reparamEval, endKey, reparam, revealed, &zero) || !LocationAtKey(spline, endKey, end))
    {
        return false;
    }
    return !(along >= revealed) || !(Distance2(end, at) >= iconDistance);
}

float IconDistance() noexcept
{
    if (g_iconName == 0)
    {
        g_iconName = players::unreal::NameValue(L"ICON_DISTANCE_FROM_LINE");
    }
    std::uint64_t scratch = 0;
    std::uint8_t  flag    = 0;
    void*         row     = nullptr;
    if (g_iconName != 0 && platform::CallGuarded(g_fn.findRow, row, g_params, g_iconName, static_cast<void*>(&scratch), &flag) &&
        row != nullptr)
    {
        return ReadField<float>(row, facts::kCraftParamValue);
    }
    return facts::kIconDistanceDefault;
}

/// The trail flag from every point measured: a bound trail near the rider, or an unbound trail's first point in range.
bool TrailFlag(const Search& search, const Vec& at)
{
    float iconDistance = -1.0f;  // read once, at the first bound trail
    for (const void* point : search.measured)
    {
        const bool notDone = ReadField<std::int32_t>(point, facts::kPointDone) == 0;
        if (players::weak_object::ObjectIn(point, facts::kPointSpline) != nullptr)
        {
            if (const void* spline = SplineOf(point); notDone && spline != nullptr)
            {
                iconDistance = iconDistance < 0.0f ? IconDistance() : iconDistance;
                if (NearTrail(spline, point, at, iconDistance))
                {
                    return true;
                }
            }
            continue;
        }
        const auto [path, count] = PathOf(point);
        if (notDone && !Named(point, facts::kPointFieldPoint) && ReadField<std::uint8_t>(point, facts::kPointHint) == 0 &&
            path != nullptr && count > 0)
        {
            const Vec   first = PathLocation(path, 0);
            const float range = ReadField<float>(point, facts::kPointRange);
            if (!(Distance3(first, at) > range) && !(Distance2(first, at) > range))
            {
                return true;
            }
        }
    }
    return false;
}

/// The hint for the rider on `body`, read only (nothing completed, bound or triggered).
Flags Evaluate(const void* manager, const void* body) noexcept
{
    Flags        flags;
    std::uint8_t endCharacter = 0;
    if (manager == nullptr || body == nullptr || !platform::CallGuarded(g_fn.isEndCharacter, endCharacter, body) ||
        endCharacter == 0)
    {
        return flags;
    }
    const void* root = ReadPointer(body, facts::kActorRootComponent);
    if (root == nullptr)
    {
        return flags;
    }
    try
    {
        const Vec  at     = LocationAt(root, facts::kComponentWorldLocation);
        const bool locked = ReadField<std::int32_t>(manager, facts::kCraftQuestLocks) > 0;
        Search     search;
        search.measured.reserve(kPointsReserved);
        Measure(manager, at, locked, nullptr, search);
        if (search.best != nullptr && FlagSet(search.best, facts::kPointCompletion))
        {
            const void* completed = search.best;
            search.best           = nullptr;
            search.distance       = kFarAway;
            search.measured.clear();
            Measure(manager, at, locked, completed, search);
        }
        if (search.best != nullptr)
        {
            Classify(search.best, at, locked, flags);
            flags.point = ReadField<std::uint64_t>(search.best, facts::kPointKey);
            flags.type  = ReadField<std::int32_t>(search.best, facts::kPointType);
            flags.done  = ReadField<std::int32_t>(search.best, facts::kPointDone);
            flags.hint  = ReadField<std::uint8_t>(search.best, facts::kPointHint);
        }
        flags.trail = TrailFlag(search, at);
    }
    catch (...)
    {
        return {};
    }
    return flags;
}

// ---- The tick's player ----

enum class Why
{
    Session,
    Rides,
    Trail,
    FirstRider,
    Nobody,
};

/// A quest trail is being revealed as its walker passes the waypoints (the tick's on-foot part does it).
bool TrailOnFoot(const void* manager) noexcept
{
    bool found = false;
    platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(manager) + facts::kCraftPoints, facts::kCraftPointElement,
                                   [&found](std::uintptr_t element) {
                                       const void* point =
                                           ReadPointer(reinterpret_cast<const void*>(element), facts::kCraftPointObject);
                                       found = point != nullptr && ReadField<std::uint8_t>(point, facts::kPointRelay) != 0 &&
                                               FlagSet(point, facts::kPointActivation);
                                       return !found;
                                   });
    return found;
}

int Choose(const void* manager, int owner, Why& why) noexcept
{
    why = owner >= 0 ? Why::Session : Why::Nobody;
    if (owner >= 0 || !players::generation::AnyActive())
    {
        return owner >= 0 ? owner : 0;
    }
    if (RideBody() != nullptr)
    {
        why = Why::Rides;
        return 0;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat) || players::generation::ChocoboOf(seat) == nullptr)
        {
            continue;
        }
        if (TrailOnFoot(manager))
        {
            why = Why::Trail;
            return 0;
        }
        why = Why::FirstRider;
        return seat;
    }
    return 0;
}

void SayTickPlayer(int player, Why why) noexcept
{
    if (player == g_tickSaid)
    {
        return;
    }
    g_tickSaid            = player;
    g_driftFrames         = 0;  // a disagreement is counted for one player
    const wchar_t* reason = why == Why::Session      ? L"their Scour or Scent session"
                            : why == Why::Rides      ? L"they ride"
                            : why == Why::Trail      ? L"a quest trail is being revealed on foot"
                            : why == Why::FirstRider ? L"player 1 walks and they ride"
                                                     : L"nobody rides";
    platform::Log(L"treasure hint: the game's own hint and Scour and Scent run as {} ({}); every other rider's hint is "
                  L"the mod's reading",
                  players::SeatName(player), reason);
}

/// The session's set made `player`'s: the set it held becomes its old player's own, `player`'s own moves in.
void Trade(void* session, int player) noexcept
{
    const IconSet held = ReadSet(session);
    WriteSet(session, g_sets[static_cast<std::size_t>(player)]);
    g_sets[static_cast<std::size_t>(g_setPlayer)] = held;
    g_sets[static_cast<std::size_t>(player)]      = {};
    g_setPlayer                                   = player;
}

/// Back to the game's one hint (the module off or failed): the session's set made `player`'s, every other set removed.
void Retire(void* manager, int player) noexcept
{
    if (g_ready && players::generation::OnGameThread())
    {
        if (void* session = Session(manager); session != nullptr && player != g_setPlayer)
        {
            Trade(session, player);
        }
        for (IconSet& set : g_sets)
        {
            if (!Empty(set))
            {
                RemoveSet(set);
            }
        }
    }
    g_setPlayer = player;
}

// ---- After the tick ----

void SayGameHint(int player, const Flags& game, const Flags& mine) noexcept
{
    if (g_gameSaidOnce && SameFlags(game, g_gameSaid))
    {
        return;
    }
    g_gameSaid     = game;
    g_gameSaidOnce = true;
    try
    {
        platform::Log(L"treasure hint: {}'s hint (the game's own): {} ({}); nearest point {}", players::SeatName(player),
                      IconName(IconOf(game)), FlagsText(game), PointText(mine));
    }
    catch (...)
    {
    }
}

/// The mod's reading for the tick's player against the game's own flags this tick.
void Compare(void* manager, int player) noexcept
{
    if (!g_gameCalled)
    {
        return;  // no call this tick: the game completed a point and returned before it
    }
    g_flags[static_cast<std::size_t>(player)] = g_gameFlags;
    Flags mine;
    auto  read = [&] { mine = Evaluate(manager, RideBody()); };
    if (!AsPlayer(player, read))
    {
        return;
    }
    SayGameHint(player, g_gameFlags, mine);
    if (SameFlags(mine, g_gameFlags))
    {
        if (g_driftFrames >= kDriftFrames)
        {
            platform::Log(L"treasure hint: the mod's reading agrees with the game's again after {} frame(s)", g_driftFrames);
        }
        g_driftFrames = 0;
        return;
    }
    if (++g_driftFrames != kDriftFrames)
    {
        return;
    }
    try
    {
        platform::LogWarning(L"treasure hint: the mod's reading disagrees with the game's for {}: the game's {}, the mod's {} "
                             L"(nearest point {}); every other rider's hint may be wrong the same way",
                             players::SeatName(player), FlagsText(g_gameFlags), FlagsText(mine), PointText(mine));
    }
    catch (...)
    {
    }
}

void SayRider(int player, const Flags& before, const Flags& now) noexcept
{
    if (SameFlags(before, now) || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"treasure hint: {}'s hint (the mod's reading): {} ({}); nearest point {}", players::SeatName(player),
                           IconName(IconOf(now)), FlagsText(now), PointText(now));
    }
    catch (...)
    {
    }
}

/// One rider other than the tick's player: their hint read and given to their own set by the game's own call, in their
/// answers; a set whose rider is gone or off their chocobo removed.
void Rider(void* manager, void* session, int player, bool shutDown) noexcept
{
    const auto index   = static_cast<std::size_t>(player);
    IconSet&   set     = g_sets[index];
    const bool empty   = Empty(set);
    const bool present = player == 0 || players::generation::Active(player);
    if (shutDown || !present)
    {
        if (!empty)
        {
            RemoveSet(set);
        }
        g_flags[index] = {};
        return;
    }
    const bool rides = player == 0 ? RideBody() != nullptr : players::generation::ChocoboOf(player) != nullptr;
    if (!rides && empty)
    {
        g_flags[index] = {};
        return;
    }
    Flags flags;
    bool  faulted = false;
    auto  update  = [&] {
        void* body = RideBody();
        if (player >= 1 && body != nullptr && body != players::generation::ChocoboOf(player) && !g_bodySaid)
        {
            g_bodySaid = true;
            platform::LogWarning(L"treasure hint: {}'s leader answer names another chocobo than theirs (said once)",
                                 players::SeatName(player));
        }
        flags = Evaluate(manager, body);
        const SwappedIn swapped(session, set);
        faulted = !platform::CallGuarded(g_fn.statusWidgets, session, static_cast<std::uint8_t>(flags.nearPoint),
                                         static_cast<void*>(nullptr), static_cast<std::uint8_t>(flags.trail),
                                         static_cast<std::uint8_t>(flags.quest), static_cast<std::uint8_t>(flags.treasure),
                                         static_cast<std::uint8_t>(flags.burrow));
    };
    if (!AsPlayer(player, update))
    {
        if (!g_refusedSaid)
        {
            g_refusedSaid = true;
            platform::LogWarning(L"treasure hint: {}'s scopes were refused for their hint (said once)", players::SeatName(player));
        }
        if (!rides && !empty)
        {
            RemoveSet(set);
        }
        return;
    }
    if (faulted)
    {
        g_faulted = true;
        platform::Log(L"treasure hint: FAILED: the game's icon update faulted for {}; players 2 and 3's hints are off",
                      players::SeatName(player));
        return;
    }
    SayRider(player, g_flags[index], flags);
    g_flags[index] = flags;
}

/// Replaces the craft tick's call of UpdateStatusWidgets: the game's own flags noted for the comparison.
void StatusCall(void* session, std::uint8_t nearPoint, void* unused, std::uint8_t trail, std::uint8_t quest,
                std::uint8_t treasure, std::uint8_t burrow) noexcept
{
    g_gameFlags           = {};
    g_gameFlags.nearPoint = nearPoint != 0;
    g_gameFlags.trail     = trail != 0;
    g_gameFlags.quest     = quest != 0;
    g_gameFlags.treasure  = treasure != 0;
    g_gameFlags.burrow    = burrow != 0;
    g_gameCalled          = true;
    reinterpret_cast<StatusWidgetsFn>(g_statusCall.Original())(session, nearPoint, unused, trail, quest, treasure, burrow);
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto found = platform::FindFunction(image, fact);
    out              = found ? reinterpret_cast<Fn>(*found) : nullptr;
    return out != nullptr;
}

/// Places the icon in the session (the seat's own swapped in by the caller) through the game's own placement.
bool PlaceSessionIcon(void* session) noexcept
{
    return platform::CallGuarded(g_fn.placeIcon, session, players::weak_object::ObjectIn(session, facts::kCraftStatusIcon),
                                 ReadField<std::int32_t>(session, facts::kCraftIconMode));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool found        = Find(image, facts::kCraftStatusWidgets, g_fn.statusWidgets);
    found             = Find(image, facts::kCraftPlaceIcon, g_fn.placeIcon) && found;
    found             = Find(image, facts::kRemoveFromParentClearZ, g_fn.remove) && found;
    found             = Find(image, facts::kLeaderRideBody, g_fn.rideBody) && found;
    found             = Find(image, facts::kIsEndCharacter, g_fn.isEndCharacter) && found;
    found             = Find(image, facts::kStoryFlag, g_fn.storyFlag) && found;
    found             = Find(image, facts::kFieldPointFind, g_fn.fieldPoint) && found;
    found             = Find(image, facts::kFindRowByName, g_fn.findRow) && found;
    found             = Find(image, facts::kSplineClosestKey, g_fn.closestKey) && found;
    found             = Find(image, facts::kSplineLocationAtKey, g_fn.locationAtKey) && found;
    found             = Find(image, facts::kSplineReparamEval, g_fn.reparamEval) && found;
    found             = Find(image, facts::kSplineLocationAtDistance, g_fn.locationAtDistance) && found;
    found             = Find(image, facts::kSplineLength, g_fn.splineLength) && found;
    const auto params = image.Rebase(facts::kCraftParams, sizeof(void*));
    g_params          = params ? reinterpret_cast<void*>(*params) : nullptr;
    g_ready = found && g_params != nullptr && players::weak_object::Known() && players::widget_opacity::Installed() &&
              g_statusCall.Install(image, facts::kCraftStatusWidgetsCall, reinterpret_cast<std::uintptr_t>(g_fn.statusWidgets),
                                   reinterpret_cast<std::uintptr_t>(&StatusCall));
    g_hidden.Reserve(players::kMaxPlayers * 3);
    platform::Log(L"treasure hint: {}",
                  g_ready ? L"ready: with Scour and Scent's tick, every rider sees the treasure hint over their own chocobo in "
                            L"their own view (the game's icon; the mod's reading of the game's test, checked against the game's own)"
                          : L"UNAVAILABLE (a check failed): only the player the game's tick runs as sees the hint");
    return g_ready;
}

void Remove() noexcept
{
    if (g_statusCall.IsInstalled() && !g_statusCall.Restore())
    {
        platform::Log(L"treasure hint: the icon update's call was changed after us; left in place (it passes through)");
    }
    if (players::generation::OnGameThread())
    {
        for (IconSet& set : g_sets)
        {
            RemoveSet(set);
        }
    }
    g_sets.fill({});
    g_flags.fill({});
    g_placedSaid.fill(false);
    g_hidden.Forget();
    g_names.clear();
    g_ready        = false;
    g_faulted      = false;
    g_setPlayer    = 0;
    g_tickSaid     = -1;
    g_driftFrames  = 0;
    g_gameSaidOnce = false;
}

int BeforeTick(void* manager, int owner) noexcept
{
    g_gameCalled = false;
    if (!g_ready || g_faulted || manager == nullptr || !players::generation::OnGameThread())
    {
        Retire(manager, owner >= 0 ? owner : 0);
        return g_setPlayer;
    }
    Why       why    = Why::Nobody;
    const int player = Choose(manager, owner, why);
    if (player != g_setPlayer)
    {
        Trade(Session(manager), player);
    }
    SayTickPlayer(player, why);
    return player;
}

void AfterTick(void* manager, int tickPlayer) noexcept
{
    void* session = Session(manager);
    if (!g_ready || g_faulted || session == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    if (tickPlayer != g_setPlayer)
    {
        Trade(session, tickPlayer);  // the tick could not run as the player chosen: the set goes with whom it ran as
    }
    Compare(manager, tickPlayer);
    const bool shutDown = ReadField<std::uint8_t>(session, facts::kCraftLifecycle) == facts::kCraftShutDown;
    for (int player = 0; player < players::kMaxPlayers && !g_faulted; ++player)
    {
        if (player != tickPlayer)
        {
            Rider(manager, session, player, shutDown);
        }
    }
}

int SessionSetPlayer() noexcept
{
    return g_setPlayer;
}

void Place(int seat) noexcept
{
    void* session = Session(scour_scent::Manager());
    if (!g_ready || g_faulted || session == nullptr || !players::SeatBuilt(seat) || !players::generation::OnGameThread())
    {
        return;
    }
    const auto index = static_cast<std::size_t>(seat);
    const bool own   = seat == g_setPlayer;
    if (IconOf(g_flags[index]) == 0 || (!own && Empty(g_sets[index])))
    {
        return;
    }
    const scopes::leader_scope::LeaderScope leader(players::generation::MemberWeakOf(seat));
    if (!leader.Held())
    {
        return;
    }
    bool placed = false;
    if (own)
    {
        placed = PlaceSessionIcon(session);
    }
    else
    {
        const SwappedIn swapped(session, g_sets[index]);
        placed = PlaceSessionIcon(session);
    }
    if (!placed)
    {
        g_faulted = true;
        platform::Log(L"treasure hint: FAILED: the game's icon placement faulted; players 2 and 3's hints are off");
        return;
    }
    if (!g_placedSaid[index])
    {
        g_placedSaid[index] = true;
        platform::LogDebug(L"treasure hint: {}'s icon placed from their own view (said once)", players::SeatName(seat));
    }
}

void ShowOnly(int seat) noexcept
{
    Restore();
    if (!g_ready || g_faulted || !players::widget_opacity::Installed() || !players::generation::OnGameThread())
    {
        return;
    }
    for (int player = 0; player < players::kMaxPlayers; ++player)
    {
        if (player == seat)
        {
            continue;
        }
        const IconSet& set = g_sets[static_cast<std::size_t>(player)];
        for (const std::uint64_t weak : {set.icon, set.arrowA, set.arrowB})
        {
            (void)g_hidden.Hide(players::weak_object::ObjectOf(weak), weak);
        }
    }
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::rides::treasure_hint
