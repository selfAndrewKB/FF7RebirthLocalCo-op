#include "hud/widget_visibility.h"

#include "facts/hud.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include <Windows.h>

namespace lss::hud::widget_visibility {

namespace {

using platform::Read;

struct VisibilityAttribute  // TAttribute<EVisibility> as the game passes it: unbound
{
    std::uint8_t  value = 0;
    std::uint8_t  isSet = 1;
    std::uint8_t  pad[6]{};
    std::uint64_t delegate = 0;
    std::int32_t  delegateSize = 0;
    std::int32_t  pad2 = 0;
};
static_assert(sizeof(VisibilityAttribute) == 24);

using SetVisibilityFn = void (*)(void* widget, VisibilityAttribute* attribute);

SetVisibilityFn g_setVisibility = nullptr;  // set once at install

bool SetGuarded(std::uintptr_t widget, std::uint8_t value) noexcept
{
    __try
    {
        VisibilityAttribute attribute;
        attribute.value = value;
        g_setVisibility(reinterpret_cast<void*>(widget), &attribute);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto setter = platform::FindFunction(image, facts::kSetVisibility);
    g_setVisibility   = setter ? reinterpret_cast<SetVisibilityFn>(*setter) : nullptr;
    return g_setVisibility != nullptr;
}

bool Installed() noexcept
{
    return g_setVisibility != nullptr;
}

bool Settable(std::uintptr_t widget) noexcept
{
    std::uintptr_t vtable   = 0;
    std::uintptr_t setter   = 0;
    std::uint64_t  delegate = 0;
    return g_setVisibility != nullptr && widget != 0 && Read(widget, vtable) &&
           Read(vtable + facts::kSetVisibilitySlot * sizeof(void*), setter) &&
           Read(widget + facts::kWidgetVisibilityDelegate, delegate) &&
           setter == reinterpret_cast<std::uintptr_t>(g_setVisibility) && delegate == 0;
}

std::uint8_t ValueOf(std::uintptr_t widget, std::uint8_t fallback) noexcept
{
    std::uint8_t value = fallback;
    Read(widget + facts::kWidgetVisibility, value);
    return value;
}

bool Set(std::uintptr_t widget, std::uint8_t value) noexcept
{
    if (widget == 0 || !Settable(widget))
    {
        return false;
    }
    return ValueOf(widget, static_cast<std::uint8_t>(value ^ 0xFF)) == value || SetGuarded(widget, value);
}

std::uintptr_t PinnedWidget(std::uintptr_t pointer) noexcept
{
    std::uintptr_t object    = 0;
    std::uintptr_t reference = 0;
    std::int32_t   shared    = 0;
    if (pointer == 0 || !Read(pointer, object) || !Read(pointer + 8, reference) || object == 0 || reference == 0 ||
        !Read(reference + facts::kSharedCount, shared) || shared <= 0)
    {
        return 0;
    }
    return object;
}

}  // namespace lss::hud::widget_visibility
