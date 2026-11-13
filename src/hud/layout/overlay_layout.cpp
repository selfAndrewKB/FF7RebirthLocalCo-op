#include "hud/layout/overlay_layout.h"

#include "facts/hud.h"
#include "hud/layout/arrow_edge_code.h"
#include "hud/layout/centered_frame_code.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/overlay_band.h"
#include "hud/layout/viewport_placement.h"
#include "hud/passes/column_hud_images.h"
#include "hud/passes/overlay_pass.h"
#include "hud/passes/ui_effect_pass.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "views/player_view_point.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

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
using SubmitLayerSlotFn       = void (*)(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused);

/// FVector2D by value: 8 bytes, so the x64 convention passes it packed in rcx, as the game's callers do.
struct Size2D
{
    float x = 0.0f;
    float y = 0.0f;
};
static_assert(sizeof(Size2D) == 8);
using ContainScaleFn = float (*)(Size2D size);

// Set once at install, then only read.
platform::CallRedirect g_rectCall;
platform::CallRedirect g_entryCall;
platform::CallRedirect g_submitCall;
// Game version 1.005 (research/RE-1005-hud-2.md): the rect call crosses a 16-byte block. The overlay renders
// on the game thread alone, so the call is placed from there, outside the render (platform/deferred_sites.h: the
// session's tick), and is only restored from there.
platform::DeferredSites g_rect;
std::uintptr_t         g_rectSite         = 0;
std::uintptr_t         g_rectTarget       = 0;
ContainScaleFn         g_containScale     = nullptr;  // the game's UI scale of a rect (facts/hud.h)
std::uintptr_t         g_overlaySingleton = 0;  // found live at install
bool                   g_installed        = false;

std::atomic<bool> g_bandEnabled{true};  // `hud-band off` turns it off for diagnosis

// Whether this render's band applies: set by the rect call, whose rect the overlay stores as its UI
// size in the same render, so it always describes the UI size the HUD reads until the next render.
// Read by the centred frame's code (the frame is the whole layout, hud/layout/centered_frame_code.h) and the
// arrow edge code (the edge by where the ray leaves, hud/layout/arrow_edge_code.h).
platform::CodeRedirect   g_frameCode;
platform::CodeRedirect   g_arrowEdgeCode;
alignas(8) std::uint8_t  g_bandApplies = 0;
LayoutSize               g_lastLayout  = kSoloLayout;  // game thread

// Each column's layout as its pass laid the overlay out: filled pass by pass (further columns first, column 0 last),
// read when column 0's pass, the frame's last, records. Game thread.
ColumnOverlay g_columnOverlay[overlay_pass::kMaxColumns];
bool          g_columnSeen[overlay_pass::kMaxColumns] = {};

void RecordColumn(const LayoutSize& layout, const OverlayBand& band) noexcept
{
    const int column = overlay_pass::IsFurtherPass() ? overlay_pass::Column() : 0;
    g_columnOverlay[column] = {layout, band};
    g_columnSeen[column]    = true;
    if (column != 0)
    {
        return;
    }
    // Column 0's pass is the frame's last, so the overlay holds its layout at the widget tick that places the UI
    // particles: each column recorded this frame gets its box for them against it (hud/passes/ui_effect_pass.h); a
    // column whose band did not apply gets none; one not recorded keeps its last.
    const LayoutPoint anchor = GlowAnchor();
    for (int i = 0; i < overlay_pass::kMaxColumns; ++i)
    {
        if (!g_columnSeen[i])
        {
            continue;
        }
        const ColumnOverlay& seen = g_columnOverlay[i];
        const auto           box  = seen.band.imageHeight > 0 ? EffectViewport(layout, seen.layout, seen.band.band, seen.band.imageWidth,
                                                                              seen.band.imageHeight, anchor)
                                                               : std::nullopt;
        ui_effect_pass::PublishColumnViewport(i, box.value_or(EffectBox{}), box.has_value());
        g_columnSeen[i] = false;
    }
}

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
    ClipMap        clip{};
};
Pending g_pending;

/// The entry the layer-entry call placed this frame, for the submit call at the end of the same
/// render: its per-child matrices are complete only then.
struct Submit
{
    bool           active = false;
    DWORD          thread = 0;
    std::uintptr_t entry  = 0;
    ClipMap        clip{};
};
Submit g_submit;
std::int32_t g_loggedMatrixCount = -1;

// What the log last reported for each pass, by its seat ([0] player 1's: the overlay renders once for each player
// while the split is shown, and their views can differ), so each change is logged once.
struct LoggedLayout
{
    IntRect image{};
    IntRect band{};
    float   viewW   = 0.0f;
    float   viewH   = 0.0f;
    int     applied = -1;
    bool    entry   = false;
};
LoggedLayout g_logged[overlay_pass::kMaxColumns];
int          g_loggedPass = 0;  // the column whose pass the rect call logged for, for the entry call that follows

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

/// Optional: the centred 1920x1080 frame as the whole layout in a band (hud/layout/centered_frame_code.h).
void InstallCenteredFrame(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kCenteredFrameSite, sizeof(kCenteredFrameStoreBytes));
    const auto resume = image.Rebase(kCenteredFrameResume);
    const auto code   = CenteredFrameCode(g_bandApplies);
    const bool held   = platform::FindFunction(image, kCenteredFrame) && site && resume &&
                      platform::PinThisModule() && g_frameCode.Install(*site, kCenteredFrameStoreBytes, code, *resume);
    platform::Log(L"HUD layout: the centred 1920x1080 frame {}", held ? L"is the whole layout in a band"
                                                                     : L"keeps the game's 16:9 box (its code differs)");
}

/// Optional: the off-screen arrows pick their edge by where the ray leaves a band (hud/layout/arrow_edge_code.h).
void InstallArrowEdge(const platform::GameImage& image) noexcept
{
    const auto site      = image.Rebase(kArrowEdgeSite, sizeof(kArrowEdgeFmaBytes));
    const auto side      = image.Rebase(kArrowEdgeSide);
    const auto topBottom = platform::FindFunction(image, kArrowEdgeTopBottom);
    const auto sixty     = image.Rebase(kArrowEdgeSixty, sizeof(float));
    const auto oneTwenty = image.Rebase(kArrowEdgeOneTwenty, sizeof(float));
    bool held = site && side && topBottom && sixty && oneTwenty &&
                platform::FindFunction(image, kArrowEdgeCheckStart) && platform::PinThisModule();
    if (held)
    {
        const auto code = ArrowEdgeCode(g_bandApplies, *side, *topBottom, *sixty, *oneTwenty);
        held = g_arrowEdgeCode.Install(*site, kArrowEdgeFmaBytes, code, *site + sizeof(kArrowEdgeFmaBytes));
    }
    platform::Log(L"HUD layout: the off-screen arrows {}", held ? L"pick their edge by where the ray leaves a band"
                                                                : L"keep the game's 16:9 edge split (its code differs)");
}

void LogOnChange(const IntRect& image, float viewW, float viewH, const IntRect& band, float host, bool enabled,
                 bool applied) noexcept
{
    g_loggedPass         = overlay_pass::IsFurtherPass() ? overlay_pass::Column() : 0;
    LoggedLayout& logged = g_logged[g_loggedPass];
    if (image == logged.image && viewW == logged.viewW && viewH == logged.viewH && band == logged.band &&
        static_cast<int>(applied) == logged.applied)
    {
        return;
    }
    logged = {image, band, viewW, viewH, applied ? 1 : 0, false};
    platform::Log(L"HUD layout (column {}'s pass): UI image {}x{}, view {:.0f}x{:.0f}; visible band ({},{})-({},{}), "
                  L"marker host scale {:.4f}: {}",
                  g_loggedPass + 1, image.Width(), image.Height(), viewW, viewH, band.minX,
                  band.minY, band.maxX, band.maxY, host,
                  applied   ? L"applied"
                  : enabled ? L"NOT applied (unusable band or scale)"
                            : L"not applied (hud-band off)");
}

/// Replaces the overlay's call to EndUI__GetCompositeLayerRect (same signature).
IntRect* OverlayLayoutRect(IntRect* out)
{
    IntRect* result    = reinterpret_cast<GetCompositeLayerRectFn>(g_rectCall.Original())(out);
    g_pending.active   = false;
    g_bandApplies = 0;
    g_lastLayout  = kSoloLayout;
    OverlayBand lastBand;  // empty while the band does not apply
    if (!views::ViewsSplit() || out == nullptr || out->minX != 0 || out->minY != 0)
    {
        RecordColumn(g_lastLayout, lastBand);
        return result;  // one view (or an unexpected rect): exactly the engine's result
    }
    std::uintptr_t overlay = 0;
    std::memcpy(&overlay, reinterpret_cast<const void*>(g_overlaySingleton), sizeof(overlay));
    if (overlay == 0)
    {
        RecordColumn(g_lastLayout, lastBand);
        return result;
    }
    const float   viewW   = ReadFloat(overlay + facts::kOverlayViewWidth);
    const float   viewH   = ReadFloat(overlay + facts::kOverlayViewHeight);
    const IntRect image   = *out;
    const IntRect band    = VisibleBand(image.Width(), image.Height(), viewW, viewH);
    const float   uiScale = g_containScale({static_cast<float>(band.Width()), static_cast<float>(band.Height())});
    const float   host    = MarkerHostScale(band, uiScale, image.Width(), image.Height(), viewW, viewH);
    const auto    place   = PlaceInBand(band, image.Width(), image.Height());
    const bool    enabled = g_bandEnabled.load(std::memory_order_relaxed);
    const bool    applied = enabled && host > 0.0f && place.scaleX != 0.0f;
    LogOnChange(image, viewW, viewH, band, host, enabled, applied);
    if (applied)
    {
        *out               = band;
        g_pending          = {true, GetCurrentThreadId(), overlay, place, host, BandClipMap(band, image.Width(), image.Height())};
        g_bandApplies = 1;
        g_lastLayout  = LayoutSizeOf(band);
        lastBand      = {band, image.Width(), image.Height()};
    }
    RecordColumn(g_lastLayout, lastBand);
    return result;
}

/// Replaces the overlay's call to FEndMenuRenderer__FindOrAddNamedLayerEntry (same register
/// arguments; the fourth is passed through unread).
void* OverlayLayerEntry(void* slot, std::uintptr_t name, IntRect* rect, std::uintptr_t unused)
{
    // A seat's pass of the overlay render names its own entry (hud/passes/column_hud_images.h).
    void* entry = reinterpret_cast<FindOrAddEntryFn>(g_entryCall.Original())(slot, EntryNameForPass(slot, name), rect,
                                                                               unused);
    if (!g_pending.active || g_pending.thread != GetCurrentThreadId() || entry == nullptr)
    {
        return entry;
    }
    g_pending.active = false;

    const auto e = reinterpret_cast<std::uintptr_t>(entry);
    if (!g_logged[g_loggedPass].entry && platform::Enabled(platform::Level::Debug))
    {
        g_logged[g_loggedPass].entry = true;
        platform::LogDebug(L"HUD layout: entry matrix x/y ({:.6f}, {:.6f}, {:.3f}, {:.3f}) -> ({:.6f}, {:.6f}, {:.3f}, "
                           L"{:.3f}); host scale {:.4f} -> {:.4f}",
                           ReadFloat(e + facts::kEntryMatrixScaleX), ReadFloat(e + facts::kEntryMatrixScaleY),
                           ReadFloat(e + facts::kEntryMatrixTranslateX), ReadFloat(e + facts::kEntryMatrixTranslateY),
                           g_pending.placement.scaleX, g_pending.placement.scaleY, g_pending.placement.translateX,
                           g_pending.placement.translateY, ReadFloat(g_pending.overlay + facts::kOverlayHostScale),
                           g_pending.hostScale);
    }
    WriteFloat(e + facts::kEntryMatrixScaleX, g_pending.placement.scaleX);
    WriteFloat(e + facts::kEntryMatrixScaleY, g_pending.placement.scaleY);
    WriteFloat(e + facts::kEntryMatrixTranslateX, g_pending.placement.translateX);
    WriteFloat(e + facts::kEntryMatrixTranslateY, g_pending.placement.translateY);
    WriteFloat(g_pending.overlay + facts::kOverlayHostScale, g_pending.hostScale);
    g_submit = {true, g_pending.thread, e, g_pending.clip};
    return entry;
}

/// Moves the per-child view-projections the entry collected this frame onto the band. Widgets
/// painted in 3D (SEndCanvas's perspective path: the field command menu, the Members panel) draw
/// with these instead of the entry matrix, and they map the layout onto the whole target.
void PlaceEntryMatrices(const Submit& submit) noexcept
{
    const auto count = *reinterpret_cast<const std::int32_t*>(submit.entry + facts::kEntryMatrixCount);
    auto*      data  = *reinterpret_cast<float* const*>(submit.entry + facts::kEntryMatrixData);
    if (count < 0 || count > facts::kEntryMatrixMaxCount || (count > 0 && data == nullptr))
    {
        if (g_loggedMatrixCount != -2)
        {
            g_loggedMatrixCount = -2;
            platform::Log(L"HUD layout: the entry's matrix list does not look like one (count {}); left alone", count);
        }
        return;
    }
    float first[16] = {};
    if (count > 0)
    {
        std::memcpy(first, data, sizeof(first));
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        ApplyClipMap(*reinterpret_cast<float(*)[16]>(data + static_cast<std::ptrdiff_t>(i) * 16), submit.clip);
    }
    if (count != g_loggedMatrixCount)
    {
        g_loggedMatrixCount = count;
        if (count > 0)
        {
            platform::LogDebug(L"HUD layout: {} per-child matrices placed in the band; the first x/y/w columns ({:.4f} "
                               L"{:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f}) -> x/y ({:.4f} {:.4f} | "
                               L"{:.4f} {:.4f})",
                               count, first[0], first[4], first[12], first[1], first[5], first[13], first[3], first[7],
                               first[15], data[0], data[12], data[5], data[13]);
        }
        else
        {
            platform::LogDebug(L"HUD layout: no per-child matrices this frame");
        }
    }
}

/// Replaces the overlay's call to FEndMenuRenderer__SubmitLayerSlot (same register arguments).
void OverlaySubmit(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused)
{
    if (g_submit.active && g_submit.thread == GetCurrentThreadId())
    {
        g_submit.active = false;
        PlaceEntryMatrices(g_submit);
    }
    if (!SubmitInThisPass(slot))
    {
        return;  // a seat's pass: player 1's pass submits the slot with every entry (hud/passes/column_hud_images.h)
    }
    reinterpret_cast<SubmitLayerSlotFn>(g_submitCall.Original())(renderer, pool, slot, unused);
}

/// The rect call: at install, or on 1.005 from the game thread between its ticks, where it crosses a block.
bool PlaceRect(const platform::GameImage&) noexcept
{
    const bool ok =
        g_rect.Place(g_rectCall, g_rectSite, g_rectTarget, reinterpret_cast<std::uintptr_t>(&OverlayLayoutRect));
    if (g_rect.FromGameThread())
    {
        platform::Log(L"HUD layout: the overlay's rect call {}",
                      ok ? L"installed from the game thread"
                         : L"could not be redirected (the HUD keeps the game's layout, cropped to each half)");
    }
    return ok;
}

}  // namespace

bool InstallOverlayLayout(const platform::GameImage& image) noexcept
{
    // Each function found only where its bytes are (platform::FindFunction logs the one that is not).
    const auto rectFn     = platform::FindFunction(image, facts::kGetCompositeLayerRect);
    const auto entryFn    = platform::FindFunction(image, facts::kFindOrAddNamedLayerEntry);
    const auto submitFn   = platform::FindFunction(image, facts::kSubmitLayerSlot);
    const auto contain    = platform::FindFunction(image, facts::kContainScale);
    const auto rectCall   = image.Rebase(facts::kOverlayRectCall, 5);
    const auto entryCall  = image.Rebase(facts::kEntryCall, 5);
    const auto submitCall = image.Rebase(facts::kSubmitCall, 5);
    const auto singleton  = players::game_globals::Address(players::game_globals::Global::RootOverlay);
    if (!rectFn || !entryFn || !submitFn || !contain || !rectCall || !entryCall || !submitCall || !singleton)
    {
        platform::Log(L"HUD layout: the overlay's code is not what the research read; the HUD keeps the game's layout");
        return false;
    }
    g_overlaySingleton = singleton;
    g_containScale     = reinterpret_cast<ContainScaleFn>(*contain);

    // The later calls first: they change nothing until the rect call has decided on a band.
    if (!g_submitCall.Install(*submitCall, *submitFn, reinterpret_cast<std::uintptr_t>(&OverlaySubmit)))
    {
        platform::Log(L"HUD layout: the overlay's submit call could not be redirected; not installed");
        return false;
    }
    if (!g_entryCall.Install(*entryCall, *entryFn, reinterpret_cast<std::uintptr_t>(&OverlayLayerEntry)))
    {
        g_submitCall.Restore();
        platform::Log(L"HUD layout: the overlay's layer-entry call could not be redirected; not installed");
        return false;
    }
    g_rectSite   = *rectCall;
    g_rectTarget = *rectFn;
    const bool crosses = platform::DeferredSites::Deferred() && (*rectCall & 0xF) + 5 > 16;
    if (!g_rect.Install(image, &PlaceRect, crosses))  // until a queued placement runs, the band is simply not applied
    {
        g_entryCall.Restore();
        g_submitCall.Restore();
        platform::Log(L"HUD layout: the overlay's rect call could not be redirected; not installed");
        return false;
    }
    g_installed = true;
    platform::Log(L"HUD layout: installed (hud-band {}){}", BandEnabled() ? L"on" : L"off",
                  g_rect.FromGameThread() ? L"; the rect call follows on the game thread" : L"");
    InstallCenteredFrame(image);
    InstallArrowEdge(image);
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
    g_pending.active = false;
    g_submit.active  = false;

    g_bandApplies = 0;
    g_rect.Cancel();  // a placement still queued does nothing
    // A rect call crossing two blocks is restored only from the game thread; elsewhere it stays (band off: it passes
    // the engine's rect through).
    const bool rect   =
        g_rect.FromGameThread() && !players::generation::OnGameThread() ? true : g_rectCall.Restore();
    const bool entry  = g_entryCall.Restore();
    const bool submit = g_submitCall.Restore();
    if (g_arrowEdgeCode.IsInstalled() && !g_arrowEdgeCode.Restore())
    {
        platform::Log(L"HUD layout: the arrow edge code was changed after us; left in place (it acts only in a band)");
    }
    if (g_frameCode.IsInstalled() && !g_frameCode.Restore())
    {
        platform::Log(L"HUD layout: the centred frame's store was changed after us; left in place (it acts only in a band)");
    }
    if (!rect || !entry || !submit)
    {
        platform::Log(L"HUD layout: could not restore {} (changed after us); left in place, passing the engine's values "
                      L"through",
                      !rect ? L"the rect call" : !entry ? L"the layer-entry call" : L"the submit call");
    }
    g_installed = !(rect && entry && submit);
}

LayoutSize LastLayoutSize() noexcept
{
    return g_lastLayout;
}

bool OverlayLayoutInstalled() noexcept
{
    return g_installed;
}

void SetBandEnabled(bool enabled) noexcept
{
    g_bandEnabled.store(enabled, std::memory_order_relaxed);
}

bool BandEnabled() noexcept
{
    return g_bandEnabled.load(std::memory_order_relaxed);
}

#if LSS_DEV

void RegisterOverlayLayoutCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"hud-band",
                       .shape = control::Shape::OnOff,
                       .help  = L"lay the HUD out in the part of its image each half shows; off logs the values only",
                       .set   = [](const Value& v) {
                           if (!OverlayLayoutInstalled())
                           {
                               return Outcome{false, L"the HUD layout is not installed (see the startup log)"};
                           }
                           const bool on = std::get<bool>(v);
                           SetBandEnabled(on);
                           // What it sets is logged at the overlay's next render while split.
                           return Outcome{true, on ? L"HUD laid out in each half's visible band (the default)"
                                                   : L"HUD full-image layout until `hud-band on` (values logged only)"};
                       },
                       .get = [] { return Value{BandEnabled()}; }});
}

#endif

}  // namespace lss::hud
