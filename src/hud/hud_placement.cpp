#include "hud/hud_placement.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "hud/hud_addresses.h"
#include "hud/viewport_placement.h"
#include "platform/log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
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
    PlacementKind  kind;
    float          anchorX;  // Area: the corner or edge of the half its 1920x1080 area goes to
    float          anchorY;
};
// A Point entry takes its anchor from the game's point (the nearest corner), so its anchorX/anchorY
// are unused and left 0.
constexpr Placement kPlacements[] = {
    {L"Compass_C", PlacementKind::Area, 0.5f, 0.0f},              // top centre
    {L"Command_C", PlacementKind::Area, 0.0f, 1.0f},              // bottom left: command menu and its prompt
    {L"BasicGuide_C", PlacementKind::Area, 0.0f, 1.0f},           // bottom left: button prompts
    {L"Status_C", PlacementKind::Area, 1.0f, 1.0f},               // bottom right: the Members panel
    {L"FieldAction_Command_C", PlacementKind::Point, 0.0f, 0.0f}, // field actions ("Call Chocobo")
};
constexpr std::uint32_t kScanTicks = 30;

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
};

// An area root whose layout the game sets again on this many ticks in a row is left to the game:
// the two would otherwise take turns every frame. Point roots have no such limit: each point the
// game writes is placed, and the game moves them often (the field-action icons slide for 8-17
// frames whenever their list is re-sorted; RE-p2-hud-roots.md §3).
constexpr std::uint32_t kMaxStreak = 5;

std::vector<Placed> g_placed;  // game thread only
std::uint32_t       g_ticks        = 0;
bool                g_active       = false;
bool                g_failedLogged = false;

int GuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

bool GuardedProcessEvent(UObject* target, UFunction* function, void* params) noexcept
{
    __try
    {
        target->ProcessEvent(function, params);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// A reflected UUserWidget setter and where its struct parameter sits in its frame.
struct Setter
{
    UFunction*   function = nullptr;
    std::int32_t value    = -1;
};

constexpr std::int32_t kFrameBytes = 64;  // every setter used here needs far less

std::optional<std::int32_t> ParamOffset(UFunction* function, const wchar_t* name, std::int32_t size)
{
    auto* property = function->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
    if (property == nullptr || property->GetSize() != size || property->GetOffset_Internal() < 0 ||
        property->GetOffset_Internal() + size > function->GetParmsSize())
    {
        return std::nullopt;
    }
    return property->GetOffset_Internal();
}

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

bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(out, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The widget's viewport layout, or nullopt if its memory cannot be read.
std::optional<ViewportLayout> ReadLayout(const UObject* widget) noexcept
{
    using namespace engine;
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
    auto* item = RC::Unreal::UObjectArray::IndexToObject(placed.index);
    return item != nullptr && item->GetUObject() == placed.widget && !item->IsUnreachable() &&
           !item->IsPendingKill() && placed.widget->GetClassPrivate() == placed.cls;
}

const Placement* PlacementOf(UObject* widget)
{
    const std::wstring name(widget->GetClassPrivate()->GetName());
    for (const auto& placement : kPlacements)
    {
        if (name == placement.className)
        {
            return &placement;
        }
    }
    return nullptr;
}

void LogPlaced(const Placed& placed)
{
    const auto& o = placed.original;
    const auto& a = placed.applied;
    platform::Log(L"HUD placement: {} anchors ({:.1f},{:.1f}) offsets ({:.0f},{:.0f},{:.0f},{:.0f}) alignment "
                  L"({:.1f},{:.1f}); the game's own: anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets "
                  L"({:.0f},{:.0f},{:.0f},{:.0f}) alignment ({:.2f},{:.2f})",
                  placed.placement->className, a.anchors[0], a.anchors[1], a.offsets[0], a.offsets[1], a.offsets[2],
                  a.offsets[3], a.alignment[0], a.alignment[1], o.anchors[0], o.anchors[1], o.anchors[2], o.anchors[3],
                  o.offsets[0], o.offsets[1], o.offsets[2], o.offsets[3], o.alignment[0], o.alignment[1]);
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
    const auto split = own ? SplitLayoutFor(placement.kind, placement.anchorX, placement.anchorY, *own) : std::nullopt;
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
        const bool isPoint    = placed.placement->kind == PlacementKind::Point;
        placed.streak         = !isPoint && g_ticks - placed.lastPlacedTick <= 2 ? placed.streak + 1 : 0;
        placed.lastPlacedTick = g_ticks;
        if (placed.streak >= kMaxStreak)
        {
            placed.managed = false;  // its layout is the game's now; nothing to set back
            platform::Log(L"HUD placement: the game sets {}'s layout every frame; left to the game",
                          placed.placement->className);
            continue;
        }
        ++placed.followed;
        Place(placed, set, !isPoint);  // a point's moves are counted, not logged one by one
    }
}

/// Every kScanTicks: forgets collected widgets and places new instances (the game creates them as
/// it needs them).
void ScanForNew(const Setters& set)
{
    std::erase_if(g_placed, [](const Placed& placed) { return !Alive(placed); });
    std::vector<UObject*> widgets;
    UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
    for (UObject* widget : widgets)
    {
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
    for (const auto& placed : g_placed)
    {
        if (placed.followed > 0)
        {
            platform::Log(L"HUD placement: {} was set by the game {} time(s) during the split; each placed again",
                          placed.placement->className, placed.followed);
        }
        if (!placed.managed || placed.applied == placed.original || !Alive(placed))
        {
            continue;  // never changed, or gone
        }
        if (ReadLayout(placed.widget) != placed.applied)
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
        if (!splitLayout && !g_active)
        {
            return;
        }
        static std::optional<Setters> setters;
        if (!setters && g_ticks++ % kScanTicks == 0)
        {
            setters = FindSetters();  // retried: UMG's reflection might not be complete yet
        }
        if (!setters)
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
            RestoreAll(*setters);
            return;
        }
        g_active = true;
        if (g_ticks++ % kScanTicks == 0)
        {
            ScanForNew(*setters);
        }
        FollowGameChanges(*setters);
    }
    catch (...)
    {
        platform::Log(L"HUD placement: failed (an exception); left as it is");
    }
}

}  // namespace lss::hud
