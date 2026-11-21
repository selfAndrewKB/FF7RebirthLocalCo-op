#include "hud/passes/ui_effect_pass.h"

#include "hud/layout/overlay_layout.h"
#include "hud/passes/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/seqlock.h"
#include "platform/settings.h"
#include "platform/verified_code.h"

#if LSS_DEV
#include "platform/command_registry.h"
#include <format>
#include <string>
#include <variant>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace lss::hud::ui_effect_pass {

namespace {

// CompositeSDRForeground_UpdateViewUB_SetViewport_ForTarget(RHICmdList, scene, view, const int32 extent[2], out): the
// composite-foreground pass's per-draw setup (research/RE-ui-particle-pass.md): the view's uniform buffer
// rewritten for the target's extent, then an FRHICommandSetViewport (0,0)-(W,H) appended to the command list. Called
// once per view by the pass's execute, right before the draw.
constexpr std::uint8_t kSetupPrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
                                           0x48, 0x8D, 0xAC, 0x24, 0x58, 0xEB, 0xFF};
constexpr platform::FunctionFact kSetup{L"CompositeSDRForeground_UpdateViewUB_SetViewport_ForTarget", 0x140C0BC94,
                                        kSetupPrologue};
/// The execute's call of it (the first of its two calls; the second is the draw).
constexpr std::uintptr_t kSetupCall = 0x140C0BC69;
/// FRHICommandListBase: the link to the last appended command's Next field.
constexpr std::uint32_t kCommandLink = 0x8;
/// FRHICommandBase: the Next field's offset inside a command (the link above points at it).
constexpr std::uint32_t kNextOffset = 0x8;
/// FRHICommandSetViewport as the setup fills it, after the vtable and Next: MinX, MinY, MinZ, MaxX, MaxY, MaxZ as
/// floats, the extent in MaxX/MaxY, MinX/MinY/MinZ 0 and MaxZ 1.
constexpr std::uint32_t kViewport = 0x10;
struct Viewport
{
    float minX, minY, minZ, maxX, maxY, maxZ;
};

using SetupFn = void* (*)(void* commandList, void* scene, void* view, const std::int32_t* extent, void* out);

// The UI effects' layout-point conversion (UIEffect_LayoutPointToEffectSpace, research/RE-summon-gauge.md): the
// overlay's UI size over its UI scale centres the point and gives the half-height it is divided by, while the sprites
// are sized in half-heights of the drawing itself. Its one read of the size, EndUI__GetRootOverlayUISize(FVector2D*
// out) returning out, fits a block; answered with the 1920x1080 frame at the overlay's UI scale while the band applies,
// so every effect's point is in the frame's half-heights, the unit its sprites are drawn in.
constexpr std::uint8_t kUiSizePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                            0x00, 0x00, 0x00, 0xE8, 0x68, 0x3D, 0x79, 0x01};
constexpr platform::FunctionFact kUiSize{L"EndUI__GetRootOverlayUISize", 0x1409D7E08, kUiSizePrologue};
constexpr std::uint8_t           kUiScalePrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x06, 0x42,
                                                       0x5A, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8D, 0x4C};
constexpr platform::FunctionFact kUiScale{L"EndUI__GetRootOverlayUIScale", 0x140BC7970, kUiScalePrologue};
constexpr std::uintptr_t         kConversionSizeCall = 0x140ECB798;  // in UIEffect_LayoutPointToEffectSpace

using UiSizeFn  = float* (*)(float* out);
using UiScaleFn = float (*)();

constexpr int kMaxColumns = overlay_pass::kMaxColumns;
/// The tallest box a column's published viewport may have, in images: two (a 16:9 column under a tall tick layout,
/// where the conversion is not answered with the frame; about one when it is) at the largest HUD size, with room for
/// the float arithmetic's rounding.
constexpr float kTallestBox = 2.0f * platform::settings::HudScaleOf(platform::settings::kHudScaleSteps - 1) * 1.001f;

enum Outcome : int
{
    kNoneYet = 0,
    kNoColumn,
    kSwitchedOff,
    kNoBand,
    kBandNotApplying,
    kCommandUnexpected,
    kSet
};

/// A column's viewport as published: the box's centre and height as fractions of the image, and whether it applies (1)
/// or not (0). A column's box is the last the overlay published for it: one whose band did not apply this frame gets
/// none, one not recorded this frame keeps its last (a column that drops out of the layout is never noted as a
/// family's again, so its box is never read).
std::array<platform::SeqFloats<4>, kMaxColumns> g_boxes;

platform::CallRedirect g_call;
std::uintptr_t         g_site   = 0;        // the live call site
std::uintptr_t         g_target = 0;        // the live engine function
std::atomic<bool>      g_on{true};          // the `hud-fx-pass` switch
std::atomic<bool>      g_removed{false};    // the module taken down: the replacement passes the engine's call through
std::atomic<bool>      g_installed{false};  // the redirect in place (the patch's own flag belongs to the installing thread)
std::atomic<bool>      g_armed{false};      // the site crosses a block: the render thread installs it
std::atomic<bool>      g_restoreAsked{false};
std::atomic<int>       g_familyColumn{-1};  // render thread: the column whose family's passes follow
std::atomic<unsigned>  g_calls{0};          // the replacement's calls and viewports set
std::atomic<unsigned>  g_applied{0};
std::atomic<int>       g_lastColumn{-1};    // what the last call saw: its column and outcome
std::atomic<int>       g_lastOutcome{kNoneYet};
std::array<Viewport, kMaxColumns> g_logged{};  // render thread: each column's last logged box (a line per change)

platform::CallRedirect g_sizeCall;               // the conversion's size read
UiScaleFn              g_uiScale = nullptr;
std::atomic<bool>      g_sizeInstalled{false};
std::atomic<unsigned>  g_frameAnswers{0};        // game thread writes: conversions answered with the frame
bool                   g_frameSaid = false;      // game thread: the first answer logged

/// The command the setup appended, read as a viewport over the whole extent and set to the column's box. False when it
/// is not that (another command, a stale link) or a copy faults: nothing is written then.
bool SetViewport(void* commandList, const std::int32_t* extent, const std::array<float, 4>& box, int column) noexcept
{
    std::int32_t   size[2] = {};
    std::uintptr_t link    = 0;
    if (commandList == nullptr || extent == nullptr || !platform::CopyGuarded(size, extent, sizeof(size)) ||
        !platform::CopyGuarded(&link, static_cast<const std::uint8_t*>(commandList) + kCommandLink, sizeof(link)) ||
        link <= 0x10000 || (link & 7u) != 0 || size[0] <= 0 || size[1] <= 0)
    {
        return false;
    }
    auto* const command = reinterpret_cast<std::uint8_t*>(link - kNextOffset);
    Viewport    viewport{};
    if (!platform::CopyGuarded(&viewport, command + kViewport, sizeof(viewport)))
    {
        return false;
    }
    // Exact: the sides are whole numbers far below 2^24, stored as floats by the setup itself.
    if (viewport.minX != 0.0f || viewport.minY != 0.0f || viewport.minZ != 0.0f || viewport.maxZ != 1.0f ||
        viewport.maxX != static_cast<float>(size[0]) || viewport.maxY != static_cast<float>(size[1]))
    {
        return false;
    }
    // The box: the published height and centre on the target, the target's shape for its width (it may reach past the
    // target's edges; the draw is clipped to the target).
    const float height  = box[2] * viewport.maxY;
    const float width   = height * viewport.maxX / viewport.maxY;
    const float centreX = box[0] * viewport.maxX;
    const float centreY = box[1] * viewport.maxY;
    Viewport    set     = viewport;
    set.minX            = centreX - width / 2.0f;
    set.minY            = centreY - height / 2.0f;
    set.maxX            = centreX + width / 2.0f;
    set.maxY            = centreY + height / 2.0f;
    if (!platform::WriteGuarded(command + kViewport, &set, sizeof(set)))
    {
        return false;
    }
    Viewport& logged = g_logged[static_cast<std::size_t>(column)];
    if (std::fabs(set.minX - logged.minX) >= 1.0f || std::fabs(set.minY - logged.minY) >= 1.0f ||
        std::fabs(set.maxX - logged.maxX) >= 1.0f || std::fabs(set.maxY - logged.maxY) >= 1.0f)
    {
        logged = set;
        platform::Log(L"UI effect pass: column {}'s UI particles placed by its box: viewport ({:.0f},{:.0f})-({:.0f},{:.0f}) "
                      L"of the {}x{} target",
                      column + 1, set.minX, set.minY, set.maxX, set.maxY, size[0], size[1]);
    }
    return true;
}

/// The replacement, render thread: the engine's setup, then the viewport command it appended set to the column's band.
void* SetupForColumn(void* commandList, void* scene, void* view, const std::int32_t* extent, void* out) noexcept
{
    void* const result = reinterpret_cast<SetupFn>(g_call.Original())(commandList, scene, view, extent, out);
    if (g_removed.load(std::memory_order_relaxed))
    {
        return result;
    }
    const int column = g_familyColumn.load(std::memory_order_relaxed);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastColumn.store(column, std::memory_order_relaxed);
    int                  outcome = kSet;
    std::array<float, 4> box{};
    if (column < 0 || column >= kMaxColumns)
    {
        outcome = kNoColumn;
    }
    else if (!g_on.load(std::memory_order_relaxed))
    {
        outcome = kSwitchedOff;
    }
    else if (!g_boxes[static_cast<std::size_t>(column)].Read(box))
    {
        outcome = kNoBand;
    }
    else if (box[3] < 0.5f)
    {
        outcome = kBandNotApplying;
    }
    else if (!SetViewport(commandList, extent, box, column))
    {
        outcome = kCommandUnexpected;
    }
    else
    {
        g_applied.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastOutcome.store(outcome, std::memory_order_relaxed);
    return result;
}

bool CrossesBlock(std::uintptr_t site) noexcept
{
    return (site & 15u) > 11u;  // five bytes from an offset above 11 run into the next aligned 16-byte block
}

/// The replacement for the conversion's size read, game thread: the overlay's own size, or while the band applies the
/// 1920x1080 frame at the overlay's own UI scale (the conversion divides the size by that scale).
float* ConversionUiSize(float* out) noexcept
{
    float* const result = reinterpret_cast<UiSizeFn>(g_sizeCall.Original())(out);
    if (out == nullptr || !ConvertsAgainstFrame() || !BandApplies())
    {
        return result;
    }
    const float scale = g_uiScale();
    if (!std::isfinite(scale) || !(scale > 0.0f))
    {
        return result;
    }
    out[0] = kSoloLayout.width * scale;
    out[1] = kSoloLayout.height * scale;
    g_frameAnswers.fetch_add(1, std::memory_order_relaxed);
    if (!std::exchange(g_frameSaid, true))
    {
        platform::Log(L"UI effect pass: the UI effects' points converted against the 1920x1080 frame while split (the "
                      L"overlay's UI scale {:.3f}), the unit their sprites are drawn in",
                      scale);
    }
    return result;
}

/// The conversion's size read redirected (the call fits a block). Logged either way.
void InstallFrameConversion(const platform::GameImage& image) noexcept
{
    const auto size  = platform::FindFunction(image, kUiSize);
    const auto scale = platform::FindFunction(image, kUiScale);
    const auto site  = image.Rebase(kConversionSizeCall, 5);
    const auto reach = image.RipTarget(kConversionSizeCall, std::array<std::uint8_t, 1>{0xE8});
    const bool ok    = size && scale && site && reach && *reach == *size && !CrossesBlock(*site) &&
                    (g_uiScale = reinterpret_cast<UiScaleFn>(*scale),
                     g_sizeCall.Install(*site, *size, reinterpret_cast<std::uintptr_t>(&ConversionUiSize)));
    g_sizeInstalled.store(ok, std::memory_order_release);
    if (ok)
    {
        platform::Log(L"UI effect pass: the UI effects' points are converted against the 1920x1080 frame while split, so "
                      L"the glow's sprites keep their size beside a tall player 1's view");
    }
    else
    {
        platform::LogWarning(L"UI effect pass: the UI effects' size read is not what the research read; their points stay "
                             L"converted against player 1's layout (the glow drawn too large beside a tall player 1's "
                             L"view)");
    }
}

}  // namespace

bool ConvertsAgainstFrame() noexcept
{
    return g_sizeInstalled.load(std::memory_order_acquire) && g_installed.load(std::memory_order_acquire) &&
           g_on.load(std::memory_order_relaxed) && !g_removed.load(std::memory_order_relaxed);
}

bool Install(const platform::GameImage& image) noexcept
{
    g_removed.store(false, std::memory_order_relaxed);
    g_restoreAsked.store(false, std::memory_order_relaxed);
    g_armed.store(false, std::memory_order_relaxed);
    g_applied.store(0, std::memory_order_relaxed);
    g_frameSaid       = false;
    const auto target = platform::FindFunction(image, kSetup);
    const auto site   = image.Rebase(kSetupCall, 5);
    const auto reach  = image.RipTarget(kSetupCall, std::array<std::uint8_t, 1>{0xE8});
    if (!target || !site || !reach || *reach != *target)
    {
        platform::Log(L"UI effect pass: UNAVAILABLE, the composite-foreground pass's setup call is not what the research "
                      L"read (function {}, site {}, the call reaches {}); the glow is drawn where the game draws it",
                      target ? L"found" : L"not found", site ? L"found" : L"not found", reach ? L"another function" : L"nothing");
        return false;
    }
    g_site   = *site;
    g_target = *target;
    if (CrossesBlock(g_site))
    {
        g_armed.store(true, std::memory_order_release);  // installed by the render thread, from the composite redirect
        platform::Log(L"UI effect pass: the UI particles follow each column's band; the call crosses a block, so the render "
                      L"thread installs it at the first frame");
        InstallFrameConversion(image);
        return true;
    }
    if (!g_call.Install(g_site, g_target, reinterpret_cast<std::uintptr_t>(&SetupForColumn)))
    {
        platform::Log(L"UI effect pass: UNAVAILABLE, the call could not be redirected");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    platform::Log(L"UI effect pass: the UI particles (the summon glow) are drawn into each column's band");
    InstallFrameConversion(image);
    return true;
}

void Remove() noexcept
{
    g_removed.store(true, std::memory_order_relaxed);
    g_armed.store(false, std::memory_order_relaxed);
    if (g_sizeInstalled.exchange(false, std::memory_order_acq_rel) && !g_sizeCall.Restore())
    {
        platform::Log(L"UI effect pass: the UI effects' size read was changed after us; left in place (it passes the "
                      L"game's size through)");
    }
    if (!g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    if (!g_call.Crosses())
    {
        const bool ok = g_call.Restore();
        g_installed.store(false, std::memory_order_release);
        platform::Log(L"UI effect pass: the engine's call {}", ok ? L"put back" : L"not ours any more; left alone");
        return;
    }
    // A crossing site: only the render thread may restore it, at its next family (the composite redirect still runs:
    // it is taken down after this); waited for briefly.
    g_restoreAsked.store(true, std::memory_order_release);
    for (int i = 0; i < 40 && g_installed.load(std::memory_order_acquire); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    platform::Log(L"UI effect pass: the engine's call {}", g_installed.load(std::memory_order_acquire)
                                                              ? L"left in place, inert (no frame came to put it back)"
                                                              : L"put back by the render thread");
}

void PublishColumnViewport(int column, const EffectBox& box, bool applies) noexcept
{
    if (column < 0 || column >= kMaxColumns)
    {
        return;
    }
    // The box within reason: a height of up to two images (a 16:9 column under a tall layout's tick, when the conversion
    // is not answered with the frame) times the largest
    // HUD size, a centre at most an image away (a column of another layout than the tick's moves the box past an edge,
    // never further).
    const bool usable = applies && std::isfinite(box.centreX) && std::isfinite(box.centreY) && std::isfinite(box.height) &&
                        box.height > 0.0f && box.height <= kTallestBox && box.centreX > -1.0f && box.centreX < 2.0f &&
                        box.centreY > -1.0f && box.centreY < 2.0f;
    g_boxes[static_cast<std::size_t>(column)].Publish(usable ? std::array<float, 4>{box.centreX, box.centreY, box.height, 1.0f}
                                                             : std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.0f});
}

void NoteFamilyColumn(int column) noexcept
{
    g_familyColumn.store(column, std::memory_order_relaxed);
    if (g_armed.exchange(false, std::memory_order_acq_rel) && !g_removed.load(std::memory_order_relaxed))
    {
        const bool ok = g_call.InstallFromOwnThread(g_site, g_target, reinterpret_cast<std::uintptr_t>(&SetupForColumn));
        g_installed.store(ok, std::memory_order_release);
        platform::Log(L"UI effect pass: {}", ok ? L"installed from the render thread; the UI particles (the summon glow) are drawn "
                                                 L"into each column's band"
                                               : L"UNAVAILABLE, the render thread could not redirect the call");
    }
    if (g_restoreAsked.exchange(false, std::memory_order_acq_rel) && g_installed.load(std::memory_order_acquire))
    {
        const bool ok = g_call.Restore();
        g_installed.store(false, std::memory_order_release);
        if (!ok)
        {
            platform::Log(L"UI effect pass: the engine's call was not ours any more; left alone");
        }
    }
}

void SetEnabled(bool on) noexcept
{
    g_on.store(on, std::memory_order_relaxed);
}

bool Enabled() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"hud-fx-pass",
                       .shape = control::Shape::OnOff,
                       .help  = L"the UI particles (the summon gauge's glow) placed per column by its box (on, the default), "
                                L"or over the whole target as the engine does (off)",
                       .set   = [](const Value& v) {
                           SetEnabled(std::get<bool>(v));
                           return Outcome{true, g_installed.load() ? L"from the next frame" : L"the pass is not redirected"};
                       },
                       .get = [] { return Value{Enabled()}; }});
    control::Register({.name  = L"hud-fx-pass.state",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the composite-foreground pass's redirect: installed, calls, viewports set, the "
                                L"column and outcome of the last call",
                       .get   = [] {
                           static constexpr const wchar_t* kOutcomes[] = {L"none yet",
                                                                          L"no column",
                                                                          L"switched off",
                                                                          L"no box published",
                                                                          L"a box that does not apply",
                                                                          L"the viewport command not as expected",
                                                                          L"set"};
                           const int outcome = std::clamp(g_lastOutcome.load(), 0, static_cast<int>(kSet));
                           const int column  = g_lastColumn.load();
                           return Value{std::format(L"installed {}; armed {}; calls {}; set {}; last column {}, {}; "
                                                    L"points converted against the 1920x1080 frame {} ({})",
                                                    g_installed.load() ? L"yes" : L"no", g_armed.load() ? L"yes" : L"no",
                                                    g_calls.load(), g_applied.load(),
                                                    column < 0 ? std::wstring(L"none") : std::to_wstring(column + 1),
                                                    kOutcomes[outcome], g_frameAnswers.load(),
                                                    ConvertsAgainstFrame() ? L"on" : L"off")};
                       }});
}

#endif

}  // namespace lss::hud::ui_effect_pass
