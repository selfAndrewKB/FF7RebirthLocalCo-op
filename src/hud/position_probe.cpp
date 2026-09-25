#include "hud/position_probe.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace lss::hud {

namespace {

// UUserWidget::execSetPositionInViewport's call to SetPositionInViewport (bytes read 2026-09-25):
//   call UUserWidget__SetPositionInViewport at 0x144CED910 (the start of its 16-byte block).
constexpr std::uintptr_t kBlueprintSetPositionCall = 0x144CED910;
constexpr std::uintptr_t kSetPositionInViewport    = 0x140CC39E8;  // (UUserWidget*, FVector2D, bool)
constexpr std::uint8_t   kSetPositionInViewportPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};

// The widget's viewport slot as UUserWidget keeps it (pass 4: anchors +0x210, offsets +0x220,
// alignment +0x230; read, never written).
constexpr std::uint32_t kViewportAnchors   = 0x210;  // FAnchors: Min, Max
constexpr std::uint32_t kViewportOffsets   = 0x220;  // FMargin: Left, Top, Right, Bottom
constexpr std::uint32_t kViewportAlignment = 0x230;  // FVector2D

constexpr int kProbeLines = 120;

using SetPositionFn = void (*)(void* widget, std::uint64_t position, std::uint64_t removeDpiScale, std::uint64_t r9);

platform::CallRedirect g_call;
bool                   g_installed = false;
std::atomic<bool>      g_on{false};
std::atomic<int>       g_linesLeft{0};

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
    if (!g_on.load(std::memory_order_relaxed) || widget == nullptr || g_linesLeft.load(std::memory_order_relaxed) <= 0 ||
        AlreadyLogged(widget, position))
    {
        return;
    }
    if (g_linesLeft.fetch_sub(1, std::memory_order_relaxed) == 1)
    {
        platform::Log(L"HUD probe: line limit reached; switch the probe off and on for more");
    }
    Log(widget, position, removeDpiScale);
}

}  // namespace

bool InstallPositionProbe(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kBlueprintSetPositionCall, 5);
    const auto target = image.Rebase(kSetPositionInViewport);
    if (!site || !target || !image.BytesMatch(kSetPositionInViewport, kSetPositionInViewportPrologue) ||
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

bool PositionProbeInstalled() noexcept
{
    return g_installed;
}

void SetPositionProbe(bool on) noexcept
{
    g_linesLeft.store(on ? kProbeLines : 0, std::memory_order_relaxed);
    g_on.store(on, std::memory_order_relaxed);
    platform::Log(L"HUD probe: {}", on ? L"on" : L"off");
}

}  // namespace lss::hud
