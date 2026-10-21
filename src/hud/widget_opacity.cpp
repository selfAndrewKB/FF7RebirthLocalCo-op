#include "hud/widget_opacity.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::hud::widget_opacity {

namespace {

// Facts read from the database (RE-p2-hud-targets.md, S-HUD22; this build).
constexpr std::uint8_t kSetOpacityBytes[] = {0x40, 0x53, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x9C, 0x8F, 0x64, 0x01};
constexpr platform::FunctionFact kSetOpacity{L"UWidget::SetRenderOpacity", 0x140A65F88, kSetOpacityBytes};
constexpr std::uint32_t kRenderOpacity = 0xC4;  // UWidget: float

using SetOpacityFn = void (*)(void* widget, float opacity);

SetOpacityFn g_setOpacity = nullptr;  // set once at install

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_setOpacity == nullptr)
    {
        const auto live = platform::FindFunction(image, kSetOpacity);
        g_setOpacity    = live ? reinterpret_cast<SetOpacityFn>(*live) : nullptr;
    }
    return g_setOpacity != nullptr;
}

bool Installed() noexcept
{
    return g_setOpacity != nullptr;
}

float Get(const void* widget) noexcept
{
    float opacity = 1.0f;  // unreadable: opaque
    platform::Read(widget, kRenderOpacity, opacity);
    return opacity;
}

bool Set(void* widget, float opacity) noexcept
{
    return g_setOpacity != nullptr && widget != nullptr && platform::CallGuarded(g_setOpacity, widget, opacity);
}

}  // namespace lss::hud::widget_opacity
