#include "hud/layout/hud_placement.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/hud.h"
#include "hud/hud_rules.h"
#include "hud/layout/glow_size.h"
#include "hud/layout/overlay_layout.h"
#include "hud/layout/viewport_placement.h"
#include "hud/user_widgets.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "players/object_slot.h"
#include "platform/log.h"
#include "players/unreal_access.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

struct Placement
{
    const wchar_t* className;
    float          anchorX;  // the corner or edge of the half its 1920x1080 area goes to
    float          anchorY;
    bool           glow = false;  // the summon time gauge: its glow's design points move with it (facts/hud.h)
};
// The field-action icons ("Call Chocobo") are not here: the game places them at points it computes
// from each icon's own base, so they keep their corner through that base (hud/field/action_icon_base.h).
constexpr Placement kPlacements[] = {
    {L"Compass_C", 0.5f, 0.0f},           // top centre
    {L"EventProgress_00_C", 0.0f, 0.0f},  // top left: the World Intel panel (its objective list and prompt)
    {L"NaviMap_End2_C", 1.0f, 0.0f},      // top right: the minimap
    {L"Command_C", 0.0f, 1.0f},           // bottom left: command menu and its prompt
    {L"BasicGuide_C", 0.0f, 1.0f},        // bottom left: button prompts
    {L"Status_C", 1.0f, 1.0f},            // bottom right: the Members panel
    // Bottom right, above the Members panel: the summon gauges (their own offset puts them at 1508,620 of the area;
    // research/RE-summon-gauge.md).
    {L"SummonChantGauge_C", 1.0f, 1.0f},  // the "SUMMON" bar that fills during a fight
    {L"SummonTimeGauge_C", 1.0f, 1.0f, true},  // the summon's name and its time left; its tick places the glow
};
constexpr std::uint32_t kScanTicks = 30;

/// The summon gauge's three glow design points (facts/hud.h), as the game had them.
struct GlowPoints
{
    LayoutPoint head;
    LayoutPoint back;
    LayoutPoint start;
    bool        operator==(const GlowPoints&) const = default;
};

struct Placed
{
    UObject*          widget = nullptr;  // followed only after Alive() confirms it
    std::int32_t      index  = -1;       // its GUObjectArray index, to confirm it is still that object
    const void*       cls    = nullptr;  // its class, so a reused slot at the same address is not taken for it
    const Placement*  placement = nullptr;
    ViewportLayout    original;          // the game's own layout, set back when the split ends
    ViewportLayout    applied;           // what was set; anything else means the game has set it since
    bool              managed = false;   // false: left alone (an unexpected layout), only remembered
    std::uint32_t     lastPlacedTick = 0;
    std::uint32_t     streak         = 0;  // re-placements on (nearly) consecutive ticks
    std::uint32_t     followed       = 0;  // game re-sets placed again (reported when the split ends)
    std::optional<GlowPoints> glow;        // a glow placement's own design points (set back when the split ends)
    GlowPoints        glowMoved;           // the points as last written (a read of anything else is the game's own write)
    LayoutSize        glowLayout;          // the layout the glow was last moved for (zero: not yet)
    OverlayBand       glowBand;            // the band it was last moved for
    bool              glowRefusedLogged = false;  // the "not design points" line, once per widget
};

// A root whose layout the game sets again on this many ticks in a row is left to the game: the two
// would otherwise take turns every frame.
constexpr std::uint32_t kMaxStreak = 5;

std::vector<Placed> g_placed;  // game thread only
std::uint32_t       g_ticks        = 0;
bool                g_active       = false;
bool                g_failedLogged = false;
bool                g_renderFollows = false;  // set once the before-render follow has run

using players::unreal::GuardedProcessEvent;
using players::unreal::ParamOffset;

/// A reflected UUserWidget setter and where its struct parameter sits in its frame.
struct Setter
{
    UFunction*   function = nullptr;
    std::int32_t value    = -1;
};

constexpr std::int32_t kFrameBytes = 64;  // every setter used here needs far less

std::optional<Setter> FindSetter(const wchar_t* path, const wchar_t* param, std::int32_t size)
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (function == nullptr || function->GetParmsSize() > kFrameBytes)
    {
        return std::nullopt;
    }
    const auto value = ParamOffset(function, param, size);
    return value ? std::optional<Setter>(Setter{function, *value}) : std::nullopt;
}

struct Setters
{
    Setter position;  // SetPositionInViewport; its bRemoveDPIScale stays false (a zeroed frame)
    Setter size;
    Setter alignment;
    Setter anchors;
};

std::optional<Setters> g_setters;  // found once UMG's reflection is complete; game thread only

std::optional<Setters> FindSetters()
{
    const auto position  = FindSetter(STR("/Script/UMG.UserWidget:SetPositionInViewport"), STR("Position"), 8);
    const auto size      = FindSetter(STR("/Script/UMG.UserWidget:SetDesiredSizeInViewport"), STR("Size"), 8);
    const auto alignment = FindSetter(STR("/Script/UMG.UserWidget:SetAlignmentInViewport"), STR("Alignment"), 8);
    const auto anchors   = FindSetter(STR("/Script/UMG.UserWidget:SetAnchorsInViewport"), STR("Anchors"), 16);
    if (!position || !size || !alignment || !anchors)
    {
        return std::nullopt;
    }
    return Setters{*position, *size, *alignment, *anchors};
}

bool Call(UObject* widget, const Setter& setter, const float* values, std::size_t count) noexcept
{
    alignas(16) std::uint8_t frame[kFrameBytes]{};
    std::memcpy(frame + setter.value, values, count * sizeof(float));
    return GuardedProcessEvent(widget, setter.function, frame);
}

/// Sets a whole viewport layout. SetPositionInViewport also resets the anchors to (0,0), so it goes
/// first and the anchors last.
bool Apply(UObject* widget, const Setters& set, const ViewportLayout& layout) noexcept
{
    const float position[2] = {layout.offsets[0], layout.offsets[1]};
    const float size[2]     = {layout.offsets[2], layout.offsets[3]};
    return Call(widget, set.position, position, 2) && Call(widget, set.size, size, 2) &&
           Call(widget, set.alignment, layout.alignment, 2) && Call(widget, set.anchors, layout.anchors, 4);
}

using platform::CopyGuarded;

/// The widget's viewport layout, or nullopt if its memory cannot be read.
std::optional<ViewportLayout> ReadLayout(const UObject* widget) noexcept
{
    using namespace facts;
    static_assert(kUserWidgetViewportOffsets == kUserWidgetViewportAnchors + sizeof(ViewportLayout::anchors) &&
                  kUserWidgetViewportAlignment == kUserWidgetViewportOffsets + sizeof(ViewportLayout::offsets) &&
                  kUserWidgetViewportLayoutEnd == kUserWidgetViewportAlignment + sizeof(ViewportLayout::alignment));
    float raw[10] = {};
    if (!CopyGuarded(raw, reinterpret_cast<const std::uint8_t*>(widget) + kUserWidgetViewportAnchors, sizeof(raw)))
    {
        return std::nullopt;
    }
    ViewportLayout layout;
    std::memcpy(layout.anchors, raw, sizeof(layout.anchors));
    std::memcpy(layout.offsets, raw + 4, sizeof(layout.offsets));
    std::memcpy(layout.alignment, raw + 8, sizeof(layout.alignment));
    return layout;
}

/// Whether `placed` is still the object it was when found: its GUObjectArray slot still holds this
/// address, not collected, and of the same class (a slot reused at the same address is another
/// object; that object is live, so reading its class is safe).
bool Alive(const Placed& placed) noexcept
{
    return players::LiveInSlot(placed.index, placed.widget) && placed.widget->GetClassPrivate() == placed.cls;
}

/// Each widget class's placement (or none), found once per class: comparing class names for every
/// widget at every scan costs far more than the scan itself.
std::unordered_map<const void*, const Placement*> g_placementOfClass;
std::unordered_map<const Placement*, std::wstring> g_loggedPlacements;  // each class's placement as last logged

const Placement* PlacementOf(UObject* widget)
{
    const void* cls = widget->GetClassPrivate();
    if (const auto found = g_placementOfClass.find(cls); found != g_placementOfClass.end())
    {
        return found->second;
    }
    const std::wstring name(widget->GetClassPrivate()->GetName());
    const Placement*   match = nullptr;
    for (const auto& placement : kPlacements)
    {
        if (name == placement.className)
        {
            match = &placement;
            break;
        }
    }
    g_placementOfClass.emplace(cls, match);
    return match;
}

/// Logs a placement when it differs from the one last logged for its class: each split places every
/// instance again with the same values.
void LogPlaced(const Placed& placed)
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const auto& o = placed.original;
    const auto& a = placed.applied;
    std::wstring text = std::format(L"{} anchors ({:.1f},{:.1f}) offsets ({:.0f},{:.0f},{:.0f},{:.0f}) alignment "
                                    L"({:.1f},{:.1f}); the game's own: anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets "
                                    L"({:.0f},{:.0f},{:.0f},{:.0f}) alignment ({:.2f},{:.2f})",
                                    placed.placement->className, a.anchors[0], a.anchors[1], a.offsets[0], a.offsets[1],
                                    a.offsets[2], a.offsets[3], a.alignment[0], a.alignment[1], o.anchors[0], o.anchors[1],
                                    o.anchors[2], o.anchors[3], o.offsets[0], o.offsets[1], o.offsets[2], o.offsets[3],
                                    o.alignment[0], o.alignment[1]);
    std::wstring& logged = g_loggedPlacements[placed.placement];
    if (text != logged)
    {
        platform::LogDebug(L"HUD placement: {}", text);
        logged = std::move(text);
    }
}

/// Stops following `placed`: its layout is left as the game (or a failed placement) has it.
void LeaveAlone(Placed& placed, const std::optional<ViewportLayout>& now) noexcept
{
    placed.managed = false;
    placed.streak  = kMaxStreak;
    placed.applied = now.value_or(placed.applied);
}

// ---- the summon gauge's glow: three design points its tick converts with the layout's size ----

using platform::WriteGuarded;

/// A design point of the 1920x1080 frame, with some slack (finite and near the frame).
bool NearFrame(const LayoutPoint& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && point.x > -400.0f && point.x < 2400.0f && point.y > -400.0f &&
           point.y < 1500.0f;
}

bool Zero(const LayoutPoint& point) noexcept
{
    return point.x == 0.0f && point.y == 0.0f;
}

/// The head's and the aura's points are set by the class (a zero one is an unbuilt widget or other data); the head's
/// starting point is zero until the game writes it (seen live: (1546,657) (1680,650) (0,0) at the first placement).
bool PlausibleGlow(const GlowPoints& points) noexcept
{
    return NearFrame(points.head) && !Zero(points.head) && NearFrame(points.back) && !Zero(points.back) &&
           NearFrame(points.start);
}

// The glow's placement in the layout: through the effects' own map (the default: the effects are drawn against the
// whole UI image, the layout into its band, viewport_placement.h EffectPointFor), or the bare layout point (the
// band rule, for comparison in the game); and a raw set of points instead (`glow-points`), to try a placement live.
enum class GlowMap
{
    Image,
    Band,
};
GlowMap                   g_glowMap = GlowMap::Image;  // game thread
std::optional<GlowPoints> g_glowRaw;                   // game thread

/// Where the game must be told to put `point` of the frame: moved as the bar was (its distance from the anchor kept in
/// the layout), then expressed as the effects' placement needs it. A zero point (not written yet) stays zero.
LayoutPoint GlowTarget(const LayoutPoint& point, const LayoutPoint& anchor, const LayoutSize& layout,
                       const OverlayBand& band) noexcept
{
    if (Zero(point))
    {
        return point;
    }
    const LayoutPoint moved = KeepDistanceFromAnchor(point, anchor, layout.width, layout.height);
    return g_glowMap == GlowMap::Band ? moved
                                      : EffectPointFor(moved, layout, band.band, band.imageWidth, band.imageHeight);
}

/// The widget's class holds the three points (the research build's layout; a class of another size is not written).
bool ClassHoldsGlow(UObject* widget) noexcept
{
    const auto* cls = widget->GetClassPrivate();
    return cls != nullptr && cls->GetPropertiesSize() >= static_cast<std::int32_t>(facts::kSummonGaugeHeadStart + sizeof(LayoutPoint));
}

bool ReadGlow(const UObject* widget, GlowPoints& out) noexcept
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(widget);
    return CopyGuarded(&out.head, bytes + facts::kSummonGaugeEffectHead, sizeof(LayoutPoint)) &&
           CopyGuarded(&out.back, bytes + facts::kSummonGaugeEffectBack, sizeof(LayoutPoint)) &&
           CopyGuarded(&out.start, bytes + facts::kSummonGaugeHeadStart, sizeof(LayoutPoint));
}

bool WriteGlow(UObject* widget, const GlowPoints& points) noexcept
{
    auto* bytes = reinterpret_cast<std::uint8_t*>(widget);
    return WriteGuarded(bytes + facts::kSummonGaugeEffectHead, &points.head, sizeof(LayoutPoint)) &&
           WriteGuarded(bytes + facts::kSummonGaugeEffectBack, &points.back, sizeof(LayoutPoint)) &&
           WriteGuarded(bytes + facts::kSummonGaugeHeadStart, &points.start, sizeof(LayoutPoint));
}

/// The gauge's glow design points moved as its bar was: each keeps its distance from the bar's corner in the current
/// layout (the tick converts them with the layout's size, so the glow lands on the bar; one particle per effect, so a
/// layout of another shape in another view is off there, the known limit). The game's own values are kept the first
/// time, each point again whenever it is not as last written (the game set it since: a construct run once more, the
/// head's starting point written at the gauges' link), and set back when the split ends. Quiet while the layout and
/// the points stand as last written.
void PlaceGlow(Placed& placed) noexcept
{
    const ColumnOverlay glow   = GlowOverlay();  // the layout shape most columns share: one particle for every column
    const LayoutSize    layout = glow.layout;
    if (layout.width <= 0.0f || layout.height <= 0.0f)
    {
        return;
    }
    GlowPoints now;
    if (!ReadGlow(placed.widget, now))
    {
        return;
    }
    if (!placed.glow)
    {
        // The game's own values, kept: the shift starts from them.
        if (!ClassHoldsGlow(placed.widget) || !PlausibleGlow(now))
        {
            if (!placed.glowRefusedLogged)
            {
                placed.glowRefusedLogged = true;
                platform::Log(L"HUD placement: {}'s glow points read ({:.0f},{:.0f}) ({:.0f},{:.0f}) ({:.0f},{:.0f}), not design "
                              L"points of its class; the glow is left where the game puts it",
                              placed.placement->className, now.head.x, now.head.y, now.back.x, now.back.y, now.start.x, now.start.y);
            }
            return;  // tried again at the next tick: the widget may not be built yet
        }
        placed.glow      = now;
        placed.glowMoved = now;
    }
    else
    {
        // A point not as it was last written is the game's own write since (a construct run again, the head's starting
        // point copied from the other gauge at their link): that point's new original; the others stay as moved.
        // A wild value (not near the frame) is not adopted: the point keeps its original.
        GlowPoints& original = *placed.glow;
        original.head        = now.head != placed.glowMoved.head && NearFrame(now.head) ? now.head : original.head;
        original.back        = now.back != placed.glowMoved.back && NearFrame(now.back) ? now.back : original.back;
        original.start       = now.start != placed.glowMoved.start && NearFrame(now.start) ? now.start : original.start;
    }
    const LayoutPoint anchor{placed.placement->anchorX, placed.placement->anchorY};
    const OverlayBand band  = glow.band;
    const GlowPoints  moved = g_glowRaw ? *g_glowRaw
                                        : GlowPoints{GlowTarget(placed.glow->head, anchor, layout, band),
                                                     GlowTarget(placed.glow->back, anchor, layout, band),
                                                     GlowTarget(placed.glow->start, anchor, layout, band)};
    if (moved == now && layout == placed.glowLayout && band == placed.glowBand)
    {
        placed.glowMoved = moved;  // in place (an adopted point under a zero shift: as written)
        return;
    }
    if (!WriteGlow(placed.widget, moved))
    {
        if (!placed.glowRefusedLogged)
        {
            placed.glowRefusedLogged = true;
            platform::Log(L"HUD placement: {}'s glow points could not be written; the glow stays where the game puts it",
                          placed.placement->className);
        }
        WriteGlow(placed.widget, *placed.glow);  // a torn write put back as far as it goes; read afresh next time
        placed.glow.reset();
        return;
    }
    placed.glowLayout = layout;
    placed.glowBand   = band;
    placed.glowMoved  = moved;
    platform::LogDebug(L"HUD placement: {}'s glow points moved for the {:.0f}x{:.0f} layout (band ({},{})-({},{}) of the "
                       L"{}x{} image; {}): head ({:.0f},{:.0f}) -> ({:.0f},{:.0f}), aura ({:.0f},{:.0f}) -> ({:.0f},{:.0f}), "
                       L"head start ({:.0f},{:.0f}) -> ({:.0f},{:.0f})",
                       placed.placement->className, layout.width, layout.height, band.band.minX, band.band.minY,
                       band.band.maxX, band.band.maxY, band.imageWidth, band.imageHeight,
                       g_glowRaw ? L"raw points" : g_glowMap == GlowMap::Band ? L"the band rule" : L"the image rule",
                       placed.glow->head.x, placed.glow->head.y, moved.head.x, moved.head.y, placed.glow->back.x,
                       placed.glow->back.y, moved.back.x, moved.back.y, placed.glow->start.x, placed.glow->start.y,
                       moved.start.x, moved.start.y);
}

/// The glow's own design points back (the split ended, or the widget's layout is left to the game).
void RestoreGlow(Placed& placed) noexcept
{
    if (placed.glow && Alive(placed) && !WriteGlow(placed.widget, *placed.glow))
    {
        platform::Log(L"HUD placement: {}'s glow points could not be set back", placed.placement->className);
    }
    placed.glow.reset();
    placed.glowLayout = {};
    placed.glowBand   = {};
    placed.glowMoved  = {};
}

/// Every placed glow kept with its bar: moved again when the layout's size changed (another view count, the band on or
/// off) or the game wrote the points again. Three guarded 8-byte reads per gauge a tick, a write only on a change.
void FollowGlowLayout() noexcept
{
    for (auto& placed : g_placed)
    {
        if (placed.placement->glow && placed.managed && Alive(placed))
        {
            PlaceGlow(placed);
        }
    }
}

/// Places `placed.widget` from its current layout, which is the game's own: this runs for a new
/// widget, and for one the game has set again since the last placement. Records what was set, and
/// logs it if `log`.
void Place(Placed& placed, const Setters& set, bool log = true)
{
    const auto own = ReadLayout(placed.widget);
    const Placement& placement = *placed.placement;
    const auto split = own ? SplitLayoutFor(placement.anchorX, placement.anchorY, *own) : std::nullopt;
    if (!split)
    {
        LeaveAlone(placed, own);
        platform::Log(L"HUD placement: {} has a layout this placement does not expect; left alone", placement.className);
        return;
    }
    placed.original = *own;
    if (*split != *own && !Apply(placed.widget, set, *split))
    {
        // Some setters may have run: put the game's layout back, so no half-set layout is ever
        // taken for the game's own, and stop following this widget.
        Apply(placed.widget, set, *own);
        LeaveAlone(placed, ReadLayout(placed.widget));
        platform::Log(L"HUD placement: a setter raised an exception for {}; its own layout set back and left alone",
                      placement.className);
        return;
    }
    const auto stored = ReadLayout(placed.widget);  // as stored, so the next comparison is exact
    if (!stored)
    {
        LeaveAlone(placed, std::nullopt);
        return;
    }
    placed.applied = *stored;
    placed.managed = true;
    if (log)
    {
        LogPlaced(placed);
    }
    if (placement.glow)
    {
        PlaceGlow(placed);
    }
}

/// Every tick: a placed widget whose layout the game has set again is placed from that layout.
void FollowGameChanges(const Setters& set)
{
    for (auto& placed : g_placed)
    {
        if (placed.streak >= kMaxStreak || !Alive(placed))
        {
            continue;
        }
        const auto now = ReadLayout(placed.widget);
        if (!now || *now == placed.applied)
        {
            continue;
        }
        placed.streak         = hud_rules::StreakAfter(g_ticks, placed.lastPlacedTick, placed.streak);
        placed.lastPlacedTick = g_ticks;
        if (placed.streak >= kMaxStreak)
        {
            placed.managed = false;  // its layout is the game's now; nothing to set back
            if (placed.placement->glow)
            {
                RestoreGlow(placed);  // the bar is where the game puts it: the glow too
            }
            platform::Log(L"HUD placement: the game sets {}'s layout every frame; left to the game",
                          placed.placement->className);
            continue;
        }
        ++placed.followed;
        Place(placed, set);
    }
}

/// Every kScanTicks: forgets collected widgets and places new instances (the game creates them as
/// it needs them).
void ScanForNew(const Setters& set)
{
    std::erase_if(g_placed, [](const Placed& placed) { return !Alive(placed); });
    for (const auto& live : user_widgets::Live())  // hud/user_widgets.h: no walk of every UObject
    {
        UObject* widget = live.object;
        const Placement* placement = PlacementOf(widget);
        const bool known = std::any_of(g_placed.begin(), g_placed.end(),
                                       [widget](const Placed& placed) { return placed.widget == widget; });
        if (placement == nullptr || known)
        {
            continue;
        }
        Placed placed{widget, static_cast<std::int32_t>(widget->GetInternalIndex()), widget->GetClassPrivate(),
                      placement};
        Place(placed, set);
        g_placed.push_back(placed);
    }
}

void RestoreAll(const Setters& set)
{
    int restored = 0;
    for (auto& placed : g_placed)
    {
        RestoreGlow(placed);
        if (placed.followed > 0)
        {
            platform::Log(L"HUD placement: {} was set by the game {} time(s) during the split; each placed again",
                          placed.placement->className, placed.followed);
        }
        const auto verdict = hud_rules::RestoreVerdict(placed.managed, placed.applied == placed.original, Alive(placed),
                                                       [&] { return ReadLayout(placed.widget) == placed.applied; });
        if (verdict == hud_rules::Restore::Skip)
        {
            continue;  // never changed, or gone
        }
        if (verdict == hud_rules::Restore::GamesOwn)
        {
            // The game set it after the last tick's check: that layout is the game's own.
            platform::Log(L"HUD placement: {} was set by the game since; left with the game's layout",
                          placed.placement->className);
            continue;
        }
        if (Apply(placed.widget, set, placed.original))
        {
            ++restored;
        }
        else
        {
            platform::Log(L"HUD placement: a setter raised an exception setting {} back", placed.placement->className);
        }
    }
    platform::Log(L"HUD placement: {} widget(s) set back to the game's own viewport layout", restored);
    g_placed.clear();
}

}  // namespace

void UpdateHudPlacement(bool splitLayout) noexcept
{
    try
    {
        if (!splitLayout)
        {
            FollowGlowSize(1.0f);  // every tick: the glow's sizes set back as soon as the split ends, and retried
        }
        if (!splitLayout && !g_active)
        {
            return;
        }
        if (!g_setters && g_ticks++ % kScanTicks == 0)
        {
            g_setters = FindSetters();  // retried: UMG's reflection might not be complete yet
        }
        if (!g_setters)
        {
            if (!g_failedLogged)
            {
                g_failedLogged = true;
                platform::Log(L"HUD placement: UUserWidget's viewport setters not found; retrying, the HUD keeps its "
                              L"own layout meanwhile");
            }
            return;
        }
        if (!splitLayout)
        {
            g_active = false;
            RestoreAll(*g_setters);
            return;
        }
        g_active = true;
        if (g_ticks++ % kScanTicks == 0)
        {
            const platform::frame_watch::Job job(L"HUD placement scan");
            ScanForNew(*g_setters);
        }
        FollowGlowLayout();
        {
            const ColumnOverlay glow = GlowOverlay();
            FollowGlowSize(EffectSizeFactor(glow.layout, glow.band.band, glow.band.imageHeight));
        }
        if (!g_renderFollows)
        {
            FollowGameChanges(*g_setters);  // no render hook: a moved widget is placed a frame late
        }
    }
    catch (...)
    {
        platform::Log(L"HUD placement: failed (an exception); left as it is");
    }
}

void FollowHudPlacementBeforeRender() noexcept
{
    try
    {
        g_renderFollows = true;
        if (g_active && g_setters)
        {
            FollowGameChanges(*g_setters);
        }
    }
    catch (...)
    {
        platform::Log(L"HUD placement: failed before the render (an exception); left as it is");
    }
}

#if LSS_DEV

void RegisterHudPlacementCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"glow-map",
                       .shape = control::Shape::Word,
                       .words = {L"image", L"band"},
                       .help  = L"how the summon gauge's glow points are moved with the bar: through the effects' own map "
                                L"against the UI image (the default), or the bare layout point (the band rule)",
                       .set   = [](const Value& v) {
                           g_glowMap = std::get<std::wstring>(v) == L"band" ? GlowMap::Band : GlowMap::Image;
                           return Outcome{true, L"moved at the next tick while split"};
                       },
                       .get = [] { return Value{std::wstring(g_glowMap == GlowMap::Band ? L"band" : L"image")}; }});
    control::Register({.name  = L"glow-points",
                       .shape = control::Shape::Text,
                       .help  = L"the summon gauge's glow points set as given, `head.x head.y aura.x aura.y` (the head's "
                                L"start keeps the map's), or `off` for the map's",
                       .set   = [](const Value& v) {
                           const std::wstring text = std::get<std::wstring>(v);
                           if (text == L"off")
                           {
                               g_glowRaw.reset();
                               return Outcome{true, L"the map's points again at the next tick"};
                           }
                           float  values[4]{};
                           size_t count = 0, at = 0;
                           while (count < 4 && at < text.size())
                           {
                               const size_t end  = text.find(L' ', at);
                               const auto   word = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
                               at                = end == std::wstring::npos ? text.size() : end + 1;
                               if (word.empty())
                               {
                                   continue;
                               }
                               const auto real = control::ReadReal(word);
                               if (!real || !NearFrame({static_cast<float>(*real), 0.0f}) || std::fabs(*real) > 100000.0)
                               {
                                   return Outcome{false, L"four numbers wanted, each within the frame's reach: head.x head.y "
                                                         L"aura.x aura.y"};
                               }
                               values[count++] = static_cast<float>(*real);
                           }
                           if (count != 4)
                           {
                               return Outcome{false, L"four numbers wanted: head.x head.y aura.x aura.y"};
                           }
                           GlowPoints raw{{values[0], values[1]}, {values[2], values[3]}, {}};
                           for (const auto& placed : g_placed)
                           {
                               if (placed.placement->glow && placed.glow)
                               {
                                   raw.start = placed.glowMoved.start;  // as the map last put it
                               }
                           }
                           g_glowRaw = raw;
                           return Outcome{true, L"set at the next tick while split"};
                       },
                       .get = [] {
                           return Value{g_glowRaw ? std::format(L"{:.0f} {:.0f} {:.0f} {:.0f}", g_glowRaw->head.x, g_glowRaw->head.y,
                                                                g_glowRaw->back.x, g_glowRaw->back.y)
                                                  : std::wstring(L"off")};
                       }});
}

#endif

}  // namespace lss::hud
