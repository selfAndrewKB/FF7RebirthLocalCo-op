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
#include "hud/layout/viewport_placement.h"
#include "hud/passes/overlay_pass.h"
#include "hud/user_widgets.h"
#include "platform/command_registry.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/settings.h"
#include "players/object_slot.h"
#include "players/seats.h"
#include "platform/log.h"
#include "players/unreal_access.h"
#include "views/window_span.h"

#include <algorithm>
#include <array>
#include <atomic>
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

/// The height of its area a root scales about when that is not its corner: kAtCorner keeps the corner.
constexpr float kAtCorner = -1.0f;
/// The summon gauges' scale height: they stand between the minimap (down to about y 314 of the frame with its ring and
/// distance labels) and the Members panel (from about y 800 with its header), their bars at y 620-690
/// (research/RE-summon-gauge.md), so 306 units of room above and 110 below. About the height that divides the frame in
/// that ratio, 1080 * 306 / 416, the gaps above and below keep that ratio at every size while the minimap grows from
/// the top and the Members panel from the bottom: at x1.50 in a view as short as the frame the gauges still fit between
/// them, where scaled about the bottom corner they ran into the minimap.
constexpr float kSummonScaleHeight = 795.0f / 1080.0f;

struct Placement
{
    const wchar_t* className;
    float          anchorX;  // the corner or edge of the half its 1920x1080 area goes to
    float          anchorY;
    bool           glow  = false;  // the summon time gauge, whose tick places the glow: the UI particles' boxes follow it
    float          scaleY = kAtCorner;  // the height of its area it is drawn at the HUD size about (its corner's x)
};
// The field-action icons ("Call Chocobo") are not here: the game places them at points it computes
// from each icon's own base, so they keep their corner through that base (hud/field/action_icon_base.h).
constexpr Placement kPlacements[] = {
    {L"Compass_C", 0.5f, 0.0f},           // top centre
    {L"EventProgress_00_C", 0.0f, 0.0f},  // top left: the World Intel panel (its objective list and prompt)
    {L"NaviMap_End2_C", 1.0f, 0.0f},      // top right: the minimap
    {L"Command_C", 0.0f, 1.0f},           // bottom left: command menu and its prompt
    // Bottom left, just above the command menu: the button prompts, sized about the same corner so the command menu
    // grows under them, never over them.
    {L"BasicGuide_C", 0.0f, 1.0f},
    {L"Status_C", 1.0f, 1.0f},            // bottom right: the Members panel
    // Bottom right, above the Members panel: the summon gauges (their own offset puts them at 1508,620 of the area;
    // research/RE-summon-gauge.md).
    {L"SummonChantGauge_C", 1.0f, 1.0f, false, kSummonScaleHeight},  // the "SUMMON" bar that fills during a fight
    // The summon's name and its time left; its tick places the glow.
    {L"SummonTimeGauge_C", 1.0f, 1.0f, true, kSummonScaleHeight},
};

/// The point of its 1920x1080 area a root is drawn at the HUD size about, as fractions of the area.
LayoutPoint ScaleCentreOf(const Placement& placement) noexcept
{
    return {placement.anchorX, placement.scaleY >= 0.0f ? placement.scaleY : placement.anchorY};
}
constexpr std::uint32_t kScanTicks = 30;

/// A root's render transform as far as the HUD size touches it: its scale and pivot, and the translation the pivot
/// accounts for (the summon gauges' own setup moves them into place with it), read only.
struct RenderScale
{
    float scale[2]       = {1.0f, 1.0f};
    float pivot[2]       = {0.5f, 0.5f};
    float translation[2] = {};

    bool SameScaleAndPivot(const RenderScale& other) const noexcept
    {
        return scale[0] == other.scale[0] && scale[1] == other.scale[1] && pivot[0] == other.pivot[0] &&
               pivot[1] == other.pivot[1];
    }
    bool SameTranslation(const RenderScale& other) const noexcept
    {
        return translation[0] == other.translation[0] && translation[1] == other.translation[1];
    }
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
    RenderScale       gameScale;              // the game's own scale and pivot, set back at size 1 and when the split ends
    RenderScale       sized;                  // what the size set, with the translation it was set for
    float             size            = 1.0f;   // player 1's HUD size set on the root (1 also while held at scale 1)
    bool              held            = false;  // the mod's pivot and scale on the root (else the game's transform)
    float             passScale       = 1.0f;   // the scale the last pass (or the tick) set on it
    bool              passSkipped     = false;  // this frame's passes leave it alone: the game set its scale meanwhile
    bool              toldPassSkip    = false;  // a pass's skip of this root has been logged
    bool              sizeLeft        = false;  // a setter failed on this root: the HUD size leaves it alone
    bool              toldOwnScale    = false;  // the game's own scale on this root has been logged
    bool              toldTranslation = false;  // the root's own translation has been logged
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

// --- The HUD size ----------------------------------------------------------------------------------------------------

// The HUD size (the settings file's [Views] hud-scale and its players' keys; research/RE-hud-size-option.md,
// RE-hud-size-per-player.md): one factor per player for their view's pinned roots in a split window, each root scaled
// about the corner it is anchored to, so it stays in that corner of its view. The roots are one set drawn again in each
// view's pass, so the tick holds them at player 1's size and each further pass sets its own player's scale just before
// its paint (SizeForPass), player 1's back before the game's own pass. World markers are other roots, not listed here,
// and keep the game's size. Set by the console's commands and the Co-op page's commits, read by the placement and the
// glow's box, all on the game thread; atomic for the console's reading.
constexpr std::array<const platform::settings::Option*, players::kMaxPlayers> kSizeOptions{
    &platform::settings::kHudScale, &platform::settings::kHudScaleTwo, &platform::settings::kHudScaleThree};
std::array<std::atomic<float>, players::kMaxPlayers> g_hudScale{1.0f, 1.0f, 1.0f};
std::array<float, players::kMaxPlayers>              g_sizeLogged{-1.0f, -1.0f, -1.0f};  // the sizes last said in effect
// Game thread: the sizes in effect as this frame's tick took them; the passes use these, never a setting changed since
// (the next tick moves the roots to it), so the frame ends at the scale the tick will compare with.
std::array<float, players::kMaxPlayers> g_tickSizes{1.0f, 1.0f, 1.0f};
// Game thread: the size each column's pass gave the roots this frame and the glow's root then (0 or 1: not held).
std::array<float, overlay_pass::kMaxColumns> g_columnSize{};
std::array<float, overlay_pass::kMaxColumns> g_columnGlow{};
std::uint32_t                                g_passSets = 0;  // the per-pass setter calls this frame

/// A player number as an index of the per-player arrays (player 1's for one out of range).
std::size_t PlayerIndex(int player) noexcept
{
    return static_cast<std::size_t>(player >= 0 && player < players::kMaxPlayers ? player : 0);
}

const platform::settings::Option& SizeOption(int player) noexcept
{
    return *kSizeOptions[PlayerIndex(player)];
}
std::array<float, overlay_pass::kMaxColumns> g_columnSaid{};   // each further column's size as last said (0: not yet)

/// UWidget's render transform setters, which hand the transform to the root's Slate widget.
struct SizeSetters
{
    Setter scale;  // SetRenderScale
    Setter pivot;  // SetRenderTransformPivot
};
std::optional<SizeSetters> g_sizeSetters;  // found once the viewport setters are; game thread only
bool                       g_sizeSettersMissingLogged = false;

std::optional<SizeSetters> FindSizeSetters()
{
    const auto scale = FindSetter(STR("/Script/UMG.Widget:SetRenderScale"), STR("Scale"), 8);
    const auto pivot = FindSetter(STR("/Script/UMG.Widget:SetRenderTransformPivot"), STR("Pivot"), 8);
    return scale && pivot ? std::optional<SizeSetters>(SizeSetters{*scale, *pivot}) : std::nullopt;
}

/// The HUD size's words, "0.75" to "1.50", as factors.
std::optional<float> FactorOf(const std::wstring& word) noexcept
{
    const auto step = platform::settings::WordIndex(platform::settings::kHudScale, word);
    return step ? std::optional<float>(platform::settings::HudScaleOf(*step)) : std::nullopt;
}

std::wstring WordOfFactor(float factor)
{
    for (std::size_t i = 0; i < platform::settings::kHudScaleWords.size(); ++i)
    {
        if (platform::settings::HudScaleOf(i) == factor)
        {
            return platform::settings::kHudScaleWords[i];
        }
    }
    return std::format(L"{:.2f}", factor);
}

std::optional<RenderScale> ReadScale(const UObject* widget) noexcept
{
    const auto* base = reinterpret_cast<const std::uint8_t*>(widget);
    RenderScale scale;
    if (!CopyGuarded(scale.scale, base + facts::kWidgetRenderScale, sizeof(scale.scale)) ||
        !CopyGuarded(scale.pivot, base + facts::kWidgetRenderPivot, sizeof(scale.pivot)) ||
        !CopyGuarded(scale.translation, base + facts::kWidgetRenderTranslation, sizeof(scale.translation)))
    {
        return std::nullopt;
    }
    return scale;
}

/// The scale and pivot through the game's setters (the translation stays the game's).
bool ApplyScale(UObject* widget, const RenderScale& scale) noexcept
{
    return Call(widget, g_sizeSetters->pivot, scale.pivot, 2) && Call(widget, g_sizeSetters->scale, scale.scale, 2);
}

/// The root at `size` about its placement's scale centre (ScaleCentreOf), its translation `now` scaled with it
/// (CornerPivot); none for an area without a size.
std::optional<RenderScale> ScaleAt(const Placed& placed, const RenderScale& now, float size) noexcept
{
    const auto pivot = CornerPivot(ScaleCentreOf(*placed.placement), {now.translation[0], now.translation[1]},
                                   placed.applied.offsets[2],
                                   placed.applied.offsets[3]);
    if (!pivot)
    {
        return std::nullopt;
    }
    return RenderScale{{size, size}, {pivot->x, pivot->y}, {now.translation[0], now.translation[1]}};
}

/// A sized root whose scale or pivot the game has set since, or one a view's pass left at that view's scale: each of the
/// two the mod set and the game has not touched goes back to the game's own (the mod's scale off a root the game gave a
/// pivot, the game's pivot back on a root it rescaled, both on a root a pass left), and the root is the game's for the
/// rest of the split.
void HandBack(Placed& placed, const RenderScale& now) noexcept
{
    // The scale the tick set, or the one the frame's last pass left (a pass that did not finish its round): the mod's.
    const bool ownScale = (now.scale[0] == placed.sized.scale[0] && now.scale[1] == placed.sized.scale[1]) ||
                          (placed.held && now.scale[0] == placed.passScale && now.scale[1] == placed.passScale);
    const bool ownPivot = now.pivot[0] == placed.sized.pivot[0] && now.pivot[1] == placed.sized.pivot[1];
    const bool put      = (!ownPivot || Call(placed.widget, g_sizeSetters->pivot, placed.gameScale.pivot, 2)) &&
                     (!ownScale || Call(placed.widget, g_sizeSetters->scale, placed.gameScale.scale, 2));
    placed.size     = 1.0f;
    placed.held     = false;
    placed.sizeLeft = true;
    if (ownScale && ownPivot)
    {
        platform::Log(L"HUD size: {} was left at a view's scale by a pass that did not finish (the game set nothing); the "
                      L"game's own transform put back{}, the root left to the game for the rest of the split",
                      placed.placement->className, put ? L"" : L" (a setter raised an exception)");
        return;
    }
    platform::Log(L"HUD size: the game set {}'s render {} since it was sized; the game's own {} put back{}, the root left to "
                  L"the game for the rest of the split",
                  placed.placement->className, ownScale ? L"pivot" : ownPivot ? L"scale" : L"scale and pivot",
                  ownScale ? L"scale" : ownPivot ? L"pivot" : L"transform kept",
                  put ? L"" : L" (a setter raised an exception)");
}

/// Sets a placed root to player 1's HUD size: held (`hold`: some player's size is not 1) at `size` about its scale
/// centre with its translation, or the game's transform back. A root the game scales itself (its render scale not 1 when
/// it would be held) is left at the game's transform and looked at again each tick; one whose scale or pivot the game set
/// since it was held is handed back (HandBack); one whose translation the game moved since gets its pivot again.
void SizeRoot(Placed& placed, float size, bool hold)
{
    if (!g_sizeSetters || placed.placement == nullptr || placed.sizeLeft || (!hold && !placed.held) || !Alive(placed))
    {
        return;
    }
    const auto now = ReadScale(placed.widget);
    if (!now)
    {
        return;
    }
    bool moved = false;
    if (placed.held && !now->SameScaleAndPivot(placed.sized))
    {
        HandBack(placed, *now);
        return;
    }
    if (placed.held && !now->SameTranslation(placed.sized))
    {
        moved = true;
        platform::LogTrace(L"HUD size: {} was moved by the game ({:.0f},{:.0f}); its pivot follows", placed.placement->className,
                           now->translation[0], now->translation[1]);
    }
    if (hold == placed.held && (!hold || size == placed.size) && !moved)
    {
        return;
    }
    if (!placed.held)
    {
        if (now->scale[0] != 1.0f || now->scale[1] != 1.0f)
        {
            if (!placed.toldOwnScale)
            {
                placed.toldOwnScale = true;
                platform::Log(L"HUD size: {} has a render scale of its own ({:.2f}x{:.2f}); left at the game's while it "
                              L"does (logged once per root)",
                              placed.placement->className, now->scale[0], now->scale[1]);
            }
            return;
        }
        placed.gameScale = *now;
    }
    const auto wanted = !hold ? std::optional(placed.gameScale) : ScaleAt(placed, *now, size);
    if (!wanted)
    {
        return;
    }
    if (ApplyScale(placed.widget, *wanted))
    {
        if (hold && (now->translation[0] != 0.0f || now->translation[1] != 0.0f) && !placed.toldTranslation)
        {
            placed.toldTranslation = true;
            platform::Log(L"HUD size: {} carries a translation of its own ({:.0f},{:.0f}); its pivot ({:.3f},{:.3f}) scales it "
                          L"about the corner with the root",
                          placed.placement->className, now->translation[0], now->translation[1], wanted->pivot[0],
                          wanted->pivot[1]);
        }
        placed.held      = hold;
        placed.size      = hold ? size : 1.0f;
        placed.sized     = *wanted;
        placed.passScale = placed.size;
        return;
    }
    ApplyScale(placed.widget, placed.gameScale);
    placed.size     = 1.0f;
    placed.held     = false;
    placed.sizeLeft = true;
    platform::Log(L"HUD size: a setter raised an exception for {}; its own transform set back", placed.placement->className);
}

/// `player`'s size in effect: their setting in a split window; a row of monitors keeps the game's size (each of its views
/// is already as big as single player's).
float SizeNow(bool splitLayout, int player) noexcept
{
    return splitLayout && !views::window_span::In() ? g_hudScale[PlayerIndex(player)].load(std::memory_order_relaxed) : 1.0f;
}
#if LSS_DEV
/// For the console: the setting, the size in effect, and each placed root's size and live transform.
std::wstring SizeReading()
{
    std::wstring text = std::format(L"settings x{:.2f}, x{:.2f}, x{:.2f}; in effect x{:.2f}, x{:.2f}, x{:.2f}; this frame's passes "
                                    L"x{:.2f}, x{:.2f}, x{:.2f} ({} setter calls); {} root(s)",
                                    g_hudScale[0].load(std::memory_order_relaxed), g_hudScale[1].load(std::memory_order_relaxed),
                                    g_hudScale[2].load(std::memory_order_relaxed), g_sizeLogged[0], g_sizeLogged[1],
                                    g_sizeLogged[2], g_columnSize[0], g_columnSize[1], g_columnSize[2], g_passSets,
                                    g_placed.size());
    for (const Placed& placed : g_placed)
    {
        text += std::format(L"; {}: size {:.2f}{}{}, pass scale {:.2f}", placed.placement->className, placed.size,
                            placed.held ? L" held" : L"", placed.sizeLeft ? L" (left)" : L"", placed.passScale);
        const auto now = Alive(placed) ? ReadScale(placed.widget) : std::nullopt;
        text += now ? std::format(L", scale {:.3f}x{:.3f} pivot ({:.3f},{:.3f}) translation ({:.1f},{:.1f})", now->scale[0],
                                  now->scale[1], now->pivot[0], now->pivot[1], now->translation[0], now->translation[1])
                    : std::wstring(L", not readable");
    }
    return text;
}
#endif

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
        SizeRoot(placed, 1.0f, false);  // the game's own scale and pivot back
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
    platform::Log(L"HUD placement: {} widget(s) set back to the game's own viewport layout and size", restored);
    g_placed.clear();
    g_sizeLogged.fill(-1.0f);  // the next split says its sizes again
    g_columnSaid.fill(0.0f);
    g_columnSize.fill(0.0f);
    g_columnGlow.fill(0.0f);
    g_tickSizes.fill(1.0f);
}

}  // namespace

LayoutPoint GlowAnchor() noexcept
{
    for (const Placement& placement : kPlacements)
    {
        if (placement.glow)
        {
            return {placement.anchorX, placement.anchorY};
        }
    }
    return {1.0f, 1.0f};
}

LayoutPoint GlowScaleCentre() noexcept
{
    for (const Placement& placement : kPlacements)
    {
        if (placement.glow)
        {
            return ScaleCentreOf(placement);
        }
    }
    return GlowAnchor();
}

float GlowSizeOf(int column) noexcept
{
    const bool known = column >= 0 && column < overlay_pass::kMaxColumns;
    return known && g_columnGlow[static_cast<std::size_t>(column)] > 0.0f ? g_columnGlow[static_cast<std::size_t>(column)] : 1.0f;
}

void SizeForPass(int column, int player) noexcept
try
{
    if (column < 0 || column >= overlay_pass::kMaxColumns || !g_active || !g_sizeSetters)
    {
        return;
    }
    const float size = g_tickSizes[PlayerIndex(player)];
    float       glow = 1.0f;
    for (Placed& placed : g_placed)
    {
        if (!placed.held || !placed.managed || placed.sizeLeft || placed.passSkipped || !Alive(placed))
        {
            continue;
        }
        const auto now = ReadScale(placed.widget);
        if (!now)
        {
            continue;
        }
        if (now->scale[0] != placed.passScale || now->scale[1] != placed.passScale)
        {
            // The game set the root's scale during the frame: the passes leave it, the next tick hands it back.
            placed.passSkipped = true;
            if (!placed.toldPassSkip)
            {
                placed.toldPassSkip = true;
                platform::Log(L"HUD size: the game set {}'s render scale during the frame ({:.2f}); each view's pass leaves it "
                              L"this frame (logged once per root and split)",
                              placed.placement->className, now->scale[0]);
            }
            continue;
        }
        if (placed.passScale != size)
        {
            const float scale[2] = {size, size};
            if (!Call(placed.widget, g_sizeSetters->scale, scale, 2))
            {
                // Whatever the faulting call left is the scale the mod set last; the next tick hands the root back.
                const auto after   = ReadScale(placed.widget);
                placed.passScale   = after ? after->scale[0] : placed.passScale;
                placed.passSkipped = true;
                continue;
            }
            placed.passScale = size;
            ++g_passSets;
        }
        glow = placed.placement->glow ? size : glow;
    }
    g_columnSize[static_cast<std::size_t>(column)] = size;
    g_columnGlow[static_cast<std::size_t>(column)] = glow;
    if (column != 0 && size != g_columnSaid[static_cast<std::size_t>(column)])
    {
        g_columnSaid[static_cast<std::size_t>(column)] = size;
        platform::Log(L"HUD size: column {}'s pass ({}'s view) draws the HUD at x{:.2f}, player 1's at x{:.2f}", column + 1,
                      players::SeatName(player), size, g_tickSizes[0]);
    }
}
catch (...)
{
}

void InstallHudSize() noexcept
try
{
    namespace settings = platform::settings;
    // A settings file from before the players' own sizes: players 2 and 3 take player 1's size once, so their HUD keeps
    // the size it had (the file reads their keys from the next start on).
    const settings::Reading& one = settings::Of(settings::kHudScale);
    for (int player = 1; player < players::kMaxPlayers; ++player)
    {
        const settings::Option& option = SizeOption(player);
        if (one.source == settings::Source::File && settings::Of(option).source == settings::Source::Default &&
            one.word != option.fallback && settings::Save(option, one.word))
        {
            platform::Log(L"HUD size: {} takes player 1's x{} from a settings file that had one size for everyone",
                          players::SeatName(player), one.word);
        }
    }
    for (int player = 0; player < players::kMaxPlayers; ++player)
    {
        const settings::Option& option = SizeOption(player);
        g_hudScale[static_cast<std::size_t>(player)].store(FactorOf(settings::Of(option).word).value_or(1.0f),
                                                           std::memory_order_relaxed);
        control::RegisterOption(
            option,
            [player](const std::wstring& word) {
                const auto factor = FactorOf(word);
                if (!factor)
                {
                    return control::Outcome{false, L"not a size from 0.75 to 1.50 in steps of 0.05"};
                }
                g_hudScale[static_cast<std::size_t>(player)].store(*factor, std::memory_order_relaxed);
                platform::Log(L"HUD size: {} x{:.2f} in their view in a split window (set)", players::SeatName(player), *factor);
                return control::Outcome{};
            },
            [player] { return WordOfFactor(g_hudScale[static_cast<std::size_t>(player)].load(std::memory_order_relaxed)); });
    }
    platform::Log(L"HUD size: player 1 x{:.2f} ({}), player 2 x{:.2f} ({}), player 3 x{:.2f} ({}), each in their own view in a "
                  L"split window",
                  g_hudScale[0].load(std::memory_order_relaxed), settings::Why(settings::kHudScale),
                  g_hudScale[1].load(std::memory_order_relaxed), settings::Why(settings::kHudScaleTwo),
                  g_hudScale[2].load(std::memory_order_relaxed), settings::Why(settings::kHudScaleThree));
#if LSS_DEV
    control::Register({.name  = L"hud.size-roots",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) each placed HUD root: the size set on it, and its live render scale, pivot and "
                                L"translation",
                       .get   = [] {
                           try
                           {
                               return control::Value{SizeReading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
#endif
}
catch (...)
{
    platform::Log(L"HUD size: the setting could not be registered (an exception); the HUD keeps the game's size");
}

void UpdateHudPlacement(bool splitLayout) noexcept
{
    try
    {
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
        if (!g_renderFollows)
        {
            FollowGameChanges(*g_setters);  // no render hook: a moved widget is placed a frame late
        }
        if (!g_sizeSetters && g_ticks % kScanTicks == 0)
        {
            g_sizeSetters = FindSizeSetters();  // retried with the scans
            if (!g_sizeSetters && !g_sizeSettersMissingLogged)
            {
                g_sizeSettersMissingLogged = true;
                platform::LogWarning(L"HUD size: UWidget's render transform setters not found; retrying, the HUD keeps the "
                                     L"game's size meanwhile");
            }
        }
        std::array<float, players::kMaxPlayers> sizes{};
        bool                                    hold = false;  // some player's size is not the game's: the roots held
        for (int player = 0; player < players::kMaxPlayers; ++player)
        {
            sizes[static_cast<std::size_t>(player)] = SizeNow(splitLayout, player);
            hold = hold || sizes[static_cast<std::size_t>(player)] != 1.0f;
        }
        if (sizes != g_sizeLogged && g_sizeSetters)
        {
            g_sizeLogged = sizes;
            platform::Log(L"HUD size: player 1 x{:.2f}, player 2 x{:.2f}, player 3 x{:.2f}, each in their own view, on the "
                          L"minimap, compass, World Intel panel, command menu, Members panel and summon gauges{}",
                          sizes[0], sizes[1], sizes[2],
                          !hold && views::window_span::In() ? L" (a row of monitors keeps the game's size)" : L"");
        }
        g_passSets  = 0;
        g_tickSizes = sizes;
        g_columnGlow.fill(0.0f);  // each column's glow size set again by its pass this frame
        for (Placed& placed : g_placed)
        {
            placed.passSkipped = false;
            // A root left to the game gets the game's transform back.
            SizeRoot(placed, placed.managed ? sizes[0] : 1.0f, placed.managed && hold);
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

}  // namespace lss::hud
