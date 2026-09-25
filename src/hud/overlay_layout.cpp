#include "hud/overlay_layout.h"

#include "hud/hud_addresses.h"
#include "hud/overlay_band.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <cstring>

namespace lss::hud {

namespace {

using GetCompositeLayerRectFn = IntRect* (*)(IntRect* out);
using FindOrAddEntryFn        = void* (*)(void* slot, std::uintptr_t name, IntRect* rect, std::uintptr_t unused);

// Set once at install, then only read.
platform::CallRedirect g_rectCall;
platform::CallRedirect g_entryCall;
std::uintptr_t         g_overlaySingleton = 0;  // live 0x1491D1948
bool                   g_installed        = false;

std::atomic<bool> g_splitActive{false};
std::atomic<bool> g_bandEnabled{false};

/// What the rect call decided for this frame's render, for the entry call a few instructions later
/// in the same function. Both run on the game thread; the entry call acts only on the thread that
/// set it, and only once.
struct Pending
{
    bool           active    = false;
    DWORD          thread    = 0;
    std::uintptr_t overlay   = 0;
    EntryPlacement placement{};
    float          hostScale = 0.0f;
};
Pending g_pending;

// What the log last reported, so each change is logged once.
IntRect g_loggedImage{};
IntRect g_loggedBand{};
float   g_loggedViewW   = 0.0f;
float   g_loggedViewH   = 0.0f;
int     g_loggedApplied = -1;
bool    g_entryLogged   = false;

float ReadFloat(std::uintptr_t address) noexcept
{
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void WriteFloat(std::uintptr_t address, float value) noexcept
{
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

void LogOnChange(const IntRect& image, float viewW, float viewH, const IntRect& band, float host, bool enabled,
                 bool applied) noexcept
{
    if (image == g_loggedImage && viewW == g_loggedViewW && viewH == g_loggedViewH && band == g_loggedBand &&
        static_cast<int>(applied) == g_loggedApplied)
    {
        return;
    }
    g_loggedImage   = image;
    g_loggedViewW   = viewW;
    g_loggedViewH   = viewH;
    g_loggedBand    = band;
    g_loggedApplied = applied ? 1 : 0;
    g_entryLogged   = false;
    platform::Log(L"HUD layout: UI image {}x{}, player 1's view {:.0f}x{:.0f}; visible band ({},{})-({},{}), marker host "
                  L"scale {:.4f}: {}",
                  image.Width(), image.Height(), viewW, viewH, band.minX, band.minY, band.maxX, band.maxY, host,
                  applied   ? L"applied"
                  : enabled ? L"NOT applied (unusable band or scale)"
                            : L"not applied (hud-band off)");
}

/// Replaces the overlay's call to EndUI__GetCompositeLayerRect (same signature).
IntRect* OverlayLayoutRect(IntRect* out)
{
    IntRect* result  = reinterpret_cast<GetCompositeLayerRectFn>(g_rectCall.Original())(out);
    g_pending.active = false;
    if (!g_splitActive.load(std::memory_order_relaxed) || out == nullptr || out->minX != 0 || out->minY != 0)
    {
        return result;  // one player (or an unexpected rect): exactly the engine's result
    }
    std::uintptr_t overlay = 0;
    std::memcpy(&overlay, reinterpret_cast<const void*>(g_overlaySingleton), sizeof(overlay));
    if (overlay == 0)
    {
        return result;
    }
    const float   viewW   = ReadFloat(overlay + engine::kOverlayViewWidth);
    const float   viewH   = ReadFloat(overlay + engine::kOverlayViewHeight);
    const IntRect image   = *out;
    const IntRect band    = VisibleBand(image.Width(), image.Height(), viewW, viewH);
    const float   host    = MarkerHostScale(band, image.Width(), image.Height(), viewW, viewH);
    const auto    place   = PlaceInBand(band, image.Width(), image.Height());
    const bool    enabled = g_bandEnabled.load(std::memory_order_relaxed);
    const bool    applied = enabled && host > 0.0f && place.scaleX != 0.0f;
    LogOnChange(image, viewW, viewH, band, host, enabled, applied);
    if (applied)
    {
        *out      = band;
        g_pending = {true, GetCurrentThreadId(), overlay, place, host};
    }
    return result;
}

/// Replaces the overlay's call to FEndMenuRenderer__FindOrAddNamedLayerEntry (same register
/// arguments; the fourth is passed through unread).
void* OverlayLayerEntry(void* slot, std::uintptr_t name, IntRect* rect, std::uintptr_t unused)
{
    void* entry = reinterpret_cast<FindOrAddEntryFn>(g_entryCall.Original())(slot, name, rect, unused);
    if (!g_pending.active || g_pending.thread != GetCurrentThreadId() || entry == nullptr)
    {
        return entry;
    }
    g_pending.active = false;

    const auto e = reinterpret_cast<std::uintptr_t>(entry);
    if (!g_entryLogged)
    {
        g_entryLogged = true;
        platform::Log(L"HUD layout: entry matrix x/y ({:.6f}, {:.6f}, {:.3f}, {:.3f}) -> ({:.6f}, {:.6f}, {:.3f}, {:.3f}); "
                      L"host scale {:.4f} -> {:.4f}",
                      ReadFloat(e + engine::kEntryMatrixScaleX), ReadFloat(e + engine::kEntryMatrixScaleY),
                      ReadFloat(e + engine::kEntryMatrixTranslateX), ReadFloat(e + engine::kEntryMatrixTranslateY),
                      g_pending.placement.scaleX, g_pending.placement.scaleY, g_pending.placement.translateX,
                      g_pending.placement.translateY, ReadFloat(g_pending.overlay + engine::kOverlayHostScale),
                      g_pending.hostScale);
    }
    WriteFloat(e + engine::kEntryMatrixScaleX, g_pending.placement.scaleX);
    WriteFloat(e + engine::kEntryMatrixScaleY, g_pending.placement.scaleY);
    WriteFloat(e + engine::kEntryMatrixTranslateX, g_pending.placement.translateX);
    WriteFloat(e + engine::kEntryMatrixTranslateY, g_pending.placement.translateY);
    WriteFloat(g_pending.overlay + engine::kOverlayHostScale, g_pending.hostScale);
    return entry;
}

}  // namespace

bool InstallOverlayLayout(const platform::GameImage& image) noexcept
{
    const bool bytesHold =
        image.BytesMatch(engine::kGetCompositeLayerRect, engine::kGetCompositeLayerRectPrologue) &&
        image.BytesMatch(engine::kFindOrAddNamedLayerEntry, engine::kFindOrAddNamedLayerEntryPrologue);
    const auto rectCall  = image.Rebase(engine::kOverlayRectCall, 5);
    const auto rectFn    = image.Rebase(engine::kGetCompositeLayerRect);
    const auto entryCall = image.Rebase(engine::kEntryCall, 5);
    const auto entryFn   = image.Rebase(engine::kFindOrAddNamedLayerEntry);
    const auto singleton = image.Rebase(engine::kRootOverlaySingleton, sizeof(void*));
    if (!bytesHold || !rectCall || !rectFn || !entryCall || !entryFn || !singleton)
    {
        platform::Log(L"HUD layout: the overlay's code is not what the research read; the HUD keeps the game's layout");
        return false;
    }
    g_overlaySingleton = *singleton;

    // The entry call first: it changes nothing until the rect call has decided on a band.
    if (!g_entryCall.Install(*entryCall, *entryFn, reinterpret_cast<std::uintptr_t>(&OverlayLayerEntry)))
    {
        platform::Log(L"HUD layout: the overlay's layer-entry call could not be redirected; not installed");
        return false;
    }
    if (!g_rectCall.Install(*rectCall, *rectFn, reinterpret_cast<std::uintptr_t>(&OverlayLayoutRect)))
    {
        g_entryCall.Restore();
        platform::Log(L"HUD layout: the overlay's rect call could not be redirected; not installed");
        return false;
    }
    g_installed = true;
    platform::Log(L"HUD layout: installed (hud-band off)");
    return true;
}

void RemoveOverlayLayout() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // Whatever stays in place afterwards must do nothing.
    g_bandEnabled.store(false, std::memory_order_relaxed);
    g_splitActive.store(false, std::memory_order_relaxed);
    g_pending.active = false;

    const bool rect  = g_rectCall.Restore();
    const bool entry = g_entryCall.Restore();
    if (!rect || !entry)
    {
        platform::Log(L"HUD layout: could not restore {} (changed after us); left in place, passing the engine's values "
                      L"through",
                      !rect && !entry ? L"either redirected call"
                      : !rect         ? L"the rect call"
                                      : L"the layer-entry call");
    }
    g_installed = !(rect && entry);
}

bool OverlayLayoutInstalled() noexcept
{
    return g_installed;
}

void SetSplitActive(bool active) noexcept
{
    g_splitActive.store(active, std::memory_order_relaxed);
}

void SetBandEnabled(bool enabled) noexcept
{
    g_bandEnabled.store(enabled, std::memory_order_relaxed);
}

bool BandEnabled() noexcept
{
    return g_bandEnabled.load(std::memory_order_relaxed);
}

}  // namespace lss::hud
