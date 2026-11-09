#include "hud/position_probe.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "facts/hud.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace lss::hud {

namespace {

// UUserWidget::execSetPositionInViewport's call to SetPositionInViewport (bytes read from the running game):
//   call UUserWidget__SetPositionInViewport at 0x144CED910 (the start of its 16-byte block).
constexpr std::uintptr_t kBlueprintSetPositionCall = 0x144CED910;

// The widget's viewport slot as UUserWidget keeps it (pass 4: anchors +0x210, offsets +0x220,
// alignment +0x230; read, never written).
constexpr std::uint32_t kViewportAnchors   = 0x210;  // FAnchors: Min, Max
constexpr std::uint32_t kViewportOffsets   = 0x220;  // FMargin: Left, Top, Right, Bottom
constexpr std::uint32_t kViewportAlignment = 0x230;  // FVector2D


using SetPositionFn = void (*)(void* widget, std::uint64_t position, std::uint64_t removeDpiScale, std::uint64_t r9);

platform::CallRedirect g_call;
bool                   g_installed = false;
std::atomic<bool>      g_on{false};

// The last position logged per widget, so a widget repeating the same position logs once.
struct Seen
{
    const void*   widget   = nullptr;
    std::uint64_t position = 0;
};
std::array<Seen, 32> g_seen{};  // game thread only
std::size_t          g_nextSeen = 0;

bool AlreadyLogged(const void* widget, std::uint64_t position) noexcept
{
    for (auto& seen : g_seen)
    {
        if (seen.widget == widget)
        {
            if (seen.position == position)
            {
                return true;
            }
            seen.position = position;
            return false;
        }
    }
    g_seen[g_nextSeen] = {widget, position};
    g_nextSeen         = (g_nextSeen + 1) % g_seen.size();
    return false;
}

std::wstring ClassName(void* widget) noexcept
{
    try
    {
        auto* object = static_cast<RC::Unreal::UObject*>(widget);
        auto* type   = object->GetClassPrivate();
        return type != nullptr ? std::wstring(type->GetName()) : std::wstring(L"?");
    }
    catch (...)
    {
        return L"?";
    }
}

void Log(void* widget, std::uint64_t position, std::uint64_t removeDpiScale) noexcept
{
    float pos[2] = {};
    std::memcpy(pos, &position, sizeof(pos));
    float slot[10] = {};
    std::memcpy(slot, static_cast<const std::byte*>(widget) + kViewportAnchors, sizeof(float) * 4);
    std::memcpy(slot + 4, static_cast<const std::byte*>(widget) + kViewportOffsets, sizeof(float) * 4);
    std::memcpy(slot + 8, static_cast<const std::byte*>(widget) + kViewportAlignment, sizeof(float) * 2);
    platform::Log(L"HUD probe: {} {} SetPositionInViewport({:.1f}, {:.1f}, removeDPI {}); anchors ({:.2f},{:.2f})-({:.2f},"
                  L"{:.2f}) offsets ({:.1f},{:.1f},{:.1f},{:.1f}) alignment ({:.2f},{:.2f})",
                  ClassName(widget), widget, pos[0], pos[1], static_cast<unsigned>(removeDpiScale & 0xFF), slot[0],
                  slot[1], slot[2], slot[3], slot[4], slot[5], slot[6], slot[7], slot[8], slot[9]);
}

/// Replaces execSetPositionInViewport's call (the same register arguments, passed through).
void BlueprintSetPosition(void* widget, std::uint64_t position, std::uint64_t removeDpiScale, std::uint64_t r9)
{
    reinterpret_cast<SetPositionFn>(g_call.Original())(widget, position, removeDpiScale, r9);
    if (!g_on.load(std::memory_order_relaxed) || widget == nullptr ||
        AlreadyLogged(widget, position))
    {
        return;
    }
    Log(widget, position, removeDpiScale);
}

}  // namespace

bool InstallPositionProbe(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kBlueprintSetPositionCall, 5);
    const auto target = platform::FindFunction(image, facts::kSetPositionInViewport);
    if (!site || !target ||
        !g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&BlueprintSetPosition)))
    {
        platform::Log(L"HUD probe: the Blueprint SetPositionInViewport call is not what the research read; no probe");
        return false;
    }
    g_installed = true;
    return true;
}

void RemovePositionProbe() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    if (g_installed && g_call.Restore())
    {
        g_installed = false;
    }
}

void SetPositionProbe(bool on) noexcept
{
    g_on.store(on, std::memory_order_relaxed);
    platform::Log(L"HUD probe: {}", on ? L"on" : L"off");
}

void RegisterPositionProbeCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"hud-probe",
                       .shape = control::Shape::OnOff,
                       .help  = L"diagnosis: the HUD position probe",
                       .set   = [](const Value& v) {
                           const bool on = std::get<bool>(v);
                           // Installed by the first switch-on, not at every dev start (its redirect sits on a
                           // Blueprint call path).
                           if (on && !g_installed)
                           {
                               const auto image = platform::GameImage::Locate();
                               if (!image || !InstallPositionProbe(*image))
                               {
                                   return Outcome{false, L"the HUD position probe could not be installed (see the "
                                                         L"log)"};
                               }
                           }
                           if (!g_installed)
                           {
                               return Outcome{true, L"the HUD probe was never installed (it is off)"};
                           }
                           SetPositionProbe(on);
                           return Outcome{};
                       },
                       .get = [] { return Value{g_installed && g_on.load(std::memory_order_relaxed)}; }});
}

}  // namespace lss::hud
