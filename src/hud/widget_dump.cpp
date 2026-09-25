#include "hud/widget_dump.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "hud/hud_addresses.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
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

constexpr int         kMaxLines = 60;
constexpr std::size_t kFrame    = 256;  // parameter frame; every function used here needs far less

using players::unreal::GuardedProcessEvent;

/// A reflected UWidget function with no inputs, and where its return value sits in the frame.
struct Getter
{
    UFunction*   function = nullptr;
    std::int32_t offset   = -1;
    std::int32_t size     = 0;
};

std::optional<Getter> FindGetter(const wchar_t* path, std::int32_t minSize) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (function == nullptr || function->GetParmsSize() > static_cast<std::int32_t>(kFrame))
    {
        return std::nullopt;
    }
    auto* property = function->FindProperty(RC::Unreal::FName(STR("ReturnValue"), RC::Unreal::FNAME_Find));
    if (property == nullptr || property->GetSize() < minSize || property->GetOffset_Internal() < 0 ||
        property->GetOffset_Internal() + property->GetSize() > function->GetParmsSize())
    {
        return std::nullopt;
    }
    return Getter{function, property->GetOffset_Internal(), property->GetSize()};
}

/// Calls `getter` on `widget` and copies `length` bytes of its return value into `out`.
bool Call(UObject* widget, const Getter& getter, void* out, std::size_t length) noexcept
{
    alignas(16) std::uint8_t frame[kFrame]{};
    if (length > static_cast<std::size_t>(getter.size) || !GuardedProcessEvent(widget, getter.function, frame))
    {
        return false;
    }
    std::memcpy(out, frame + getter.offset, length);
    return true;
}

struct Row
{
    float        area = 0.0f;
    std::wstring line;
};

constexpr int         kMaxSlotDepth = 4;
constexpr int         kMaxSlotLines = 80;
constexpr std::int32_t kAnchorDataSize = 40;  // FAnchorData: FMargin Offsets, FAnchors Anchors, FVector2D Alignment

std::wstring Describe(UObject* object)
{
    if (object == nullptr)
    {
        return L"(none)";
    }
    return std::wstring(object->GetClassPrivate()->GetName()) + L" '" + std::wstring(object->GetName()) + L"'";
}

/// A reflected object-pointer property of `object`, or null.
UObject* ObjectProperty(UObject* object, const wchar_t* name)
{
    auto* property = object->GetPropertyByNameInChain(name);
    if (property == nullptr || property->GetSize() != static_cast<std::int32_t>(sizeof(void*)))
    {
        return nullptr;
    }
    return *object->GetValuePtrByPropertyNameInChain<UObject*>(name);
}

void WalkSlots(UObject* widget, int depth, int& lines)
{
    if (widget == nullptr || depth > kMaxSlotDepth || lines >= kMaxSlotLines)
    {
        return;
    }
    // A user widget: continue into its own tree.
    if (UObject* tree = ObjectProperty(widget, STR("WidgetTree")))
    {
        WalkSlots(ObjectProperty(tree, STR("RootWidget")), depth + 1, lines);
        return;
    }
    // A panel: every slot, then its content.
    if (widget->GetPropertyByNameInChain(STR("Slots")) == nullptr)
    {
        return;
    }
    auto* slots = widget->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("Slots"));
    for (std::int32_t i = 0; slots != nullptr && i < slots->Num() && lines < kMaxSlotLines; ++i)
    {
        UObject* slot    = (*slots)[i];
        UObject* content = slot != nullptr ? ObjectProperty(slot, STR("Content")) : nullptr;
        std::wstring layout;
        if (slot != nullptr)
        {
            auto* data = slot->GetPropertyByNameInChain(STR("LayoutData"));
            if (data != nullptr && data->GetSize() == kAnchorDataSize)
            {
                float v[10] = {};
                std::memcpy(v, slot->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("LayoutData")), sizeof(v));
                const auto* autoSize = slot->GetPropertyByNameInChain(STR("bAutoSize")) != nullptr
                                           ? slot->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("bAutoSize"))
                                           : nullptr;
                layout = std::format(L" anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets ({:.1f},{:.1f},{:.1f},{:.1f}) "
                                     L"alignment ({:.2f},{:.2f}) autosize {}",
                                     v[4], v[5], v[6], v[7], v[0], v[1], v[2], v[3], v[8], v[9],
                                     autoSize != nullptr ? (*autoSize & 1) : -1);
            }
        }
        platform::Log(L"dump slots: {}{} -> {}{}", std::wstring(static_cast<std::size_t>(depth) * 2, L' '),
                      slot != nullptr ? std::wstring(slot->GetClassPrivate()->GetName()) : L"(null slot)",
                      Describe(content), layout);
        ++lines;
        WalkSlots(content, depth + 1, lines);
    }
}

/// Where a user widget is: visible or not, its cached geometry, and its viewport layout (anchors,
/// offsets, alignment; hud_addresses.h).
void LogPlacement(UObject* widget)
{
    static const auto isVisible = FindGetter(STR("/Script/UMG.Widget:IsVisible"), 1);
    static const auto geometry  = FindGetter(STR("/Script/UMG.Widget:GetCachedGeometry"), 20);
    bool  visible = false;
    float geom[5] = {};
    if (!isVisible || !geometry || !Call(widget, *isVisible, &visible, 1) || !Call(widget, *geometry, geom, sizeof(geom)))
    {
        return;
    }
    float v[10] = {};
    static_assert(sizeof(v) == engine::kUserWidgetViewportLayoutEnd - engine::kUserWidgetViewportAnchors);
    std::memcpy(v, reinterpret_cast<const std::uint8_t*>(widget) + engine::kUserWidgetViewportAnchors, sizeof(v));
    platform::Log(L"dump slots:   visible {}; geometry size {:.0f}x{:.0f} scale {:.3f} at ({:.0f}, {:.0f}); viewport "
                  L"anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets ({:.0f},{:.0f},{:.0f},{:.0f}) alignment ({:.2f},{:.2f})",
                  visible, geom[0], geom[1], geom[2], geom[3], geom[4], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                  v[8], v[9]);
}

}  // namespace

int DumpSlots(const std::wstring& className) noexcept
{
    try
    {
        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        int lines = 0;
        int found = 0;
        for (UObject* widget : widgets)
        {
            if (std::wstring(widget->GetClassPrivate()->GetName()) != className)
            {
                continue;
            }
            ++found;
            UObject* tree = ObjectProperty(widget, STR("WidgetTree"));
            UObject* slot = ObjectProperty(widget, STR("Slot"));  // null for a widget on the viewport itself
            std::wstring outers;
            for (UObject* outer = widget->GetOuterPrivate(); outer != nullptr && outers.size() < 400;
                 outer = outer->GetOuterPrivate())
            {
                outers += L" < " + Describe(outer);
            }
            platform::Log(L"dump slots: {} (root {}); its slot {} in {}; outers{}", Describe(widget),
                          Describe(tree != nullptr ? ObjectProperty(tree, STR("RootWidget")) : nullptr), Describe(slot),
                          Describe(slot != nullptr ? ObjectProperty(slot, STR("Parent")) : nullptr), outers);
            LogPlacement(widget);
            WalkSlots(widget, 0, lines);
        }
        if (found == 0)
        {
            platform::Log(L"dump slots: no live widget of class {}", className);
        }
        return lines;
    }
    catch (...)
    {
        platform::Log(L"dump slots: failed (an exception while walking)");
        return -1;
    }
}

int DumpWidgets() noexcept
{
    try
    {
        const auto isVisible = FindGetter(STR("/Script/UMG.Widget:IsVisible"), 1);
        const auto desired   = FindGetter(STR("/Script/UMG.Widget:GetDesiredSize"), 8);
        const auto geometry  = FindGetter(STR("/Script/UMG.Widget:GetCachedGeometry"), 20);
        if (!isVisible || !desired || !geometry)
        {
            platform::Log(L"dump widgets: UWidget's IsVisible, GetDesiredSize or GetCachedGeometry not found");
            return -1;
        }

        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        std::vector<Row> rows;
        for (UObject* widget : widgets)
        {
            bool visible = false;
            if (!Call(widget, *isVisible, &visible, 1) || !visible)
            {
                continue;
            }
            float want[2] = {};
            // FGeometry begins Size, Scale, AbsolutePosition (UE4 Layout/Geometry.h); read as floats.
            float geom[5] = {};
            if (!Call(widget, *desired, want, sizeof(want)) || !Call(widget, *geometry, geom, sizeof(geom)))
            {
                continue;
            }
            if (geom[0] <= 0.0f || geom[1] <= 0.0f)
            {
                continue;  // never arranged (not on screen)
            }
            Row row;
            row.area = geom[0] * geom[1];
            row.line = std::format(L"dump widgets: {} '{}': desired {:.0f}x{:.0f}; geometry size {:.0f}x{:.0f} scale {:.3f} "
                                   L"at ({:.0f}, {:.0f})",
                                   std::wstring(widget->GetClassPrivate()->GetName()), std::wstring(widget->GetName()),
                                   want[0], want[1], geom[0], geom[1], geom[2], geom[3], geom[4]);
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.area > b.area; });
        platform::Log(L"dump widgets: {} user widgets, {} visible and arranged{}", widgets.size(), rows.size(),
                      rows.size() > kMaxLines ? L" (largest listed)" : L"");
        const int count = static_cast<int>(std::min<std::size_t>(rows.size(), kMaxLines));
        for (int i = 0; i < count; ++i)
        {
            platform::Log(L"{}", rows[static_cast<std::size_t>(i)].line);
        }
        return count;
    }
    catch (...)
    {
        platform::Log(L"dump widgets: failed (an exception while listing)");
        return -1;
    }
}

}  // namespace lss::hud
