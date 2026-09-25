#include "hud/widget_dump.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/log.h"

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
    if (property == nullptr || property->GetSize() < minSize)
    {
        return std::nullopt;
    }
    return Getter{function, property->GetOffset_Internal(), property->GetSize()};
}

/// Calls `getter` on `widget` and copies `length` bytes of its return value into `out`.
bool Call(UObject* widget, const Getter& getter, void* out, std::size_t length) noexcept
{
    alignas(16) std::uint8_t frame[kFrame]{};
    if (!GuardedProcessEvent(widget, getter.function, frame))
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

}  // namespace

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
