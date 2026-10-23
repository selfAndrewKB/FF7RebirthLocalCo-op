#include "renderer/view_family_submission.h"

#include "facts/engine.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "renderer/family_boundary.h"
#include "renderer/one_family_per_view.h"
#include "renderer/scene_view_family.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

namespace lss::renderer {

namespace {

// Set once by InstallViewFamilySubmission before the entry is replaced, then only read. The
// replacement runs on the game thread only (Draw and UMG submit from it), so nothing here
// needs a lock.
platform::VTableEntry g_entry;
SubmitViewFamilyFn    g_engineSubmit = nullptr;
std::uintptr_t        g_drawReturn   = 0;

// ---------------------------------------------------------------------------------------------
// What the log records about the game viewport's submissions
// ---------------------------------------------------------------------------------------------
//
// Each time the number of views or any view's output rectangle changes (a player joins or
// leaves, the resolution or the split layout changes), one block: the view count, the values the
// engine itself put in +0x70 and +0x74 (before the mod sets them per family), and each view's
// output rectangle, which confirms view k belongs to player k. Nothing is logged on frames where
// nothing changed. The rectangles are compared too, because the first frame after startup can
// still have the pre-display-mode size.

constexpr std::int32_t kRectLoggedViews = 4;  // views beyond these are logged but not compared

using ViewRect = std::array<std::int32_t, 4>;  // Min.X, Min.Y, Max.X, Max.Y

std::int32_t g_loggedViewCount = -1;
ViewRect     g_loggedRects[kRectLoggedViews]{};

/// The family's view list: an array of `const FSceneView*`.
const std::byte* const* Views(const std::byte* viewFamily) noexcept
{
    return *reinterpret_cast<const std::byte* const* const*>(viewFamily + family::kViewsData);
}

ViewRect RectOf(const std::byte* sceneView) noexcept
{
    ViewRect rect{};
    std::memcpy(rect.data(), sceneView + view::kUnscaledViewRect, sizeof(rect));
    return rect;
}

bool RectsUnchanged(const std::byte* const* views, std::int32_t count) noexcept
{
    for (std::int32_t k = 0; views != nullptr && k < (std::min)(count, kRectLoggedViews); ++k)
    {
        if (RectOf(views[k]) != g_loggedRects[k])
        {
            return false;
        }
    }
    return true;
}

void LogFamilyOnChange(const std::byte* viewFamily) noexcept
{
    const auto count = *reinterpret_cast<const std::int32_t*>(viewFamily + family::kViewsCount);
    const auto views = Views(viewFamily);
    if (count == g_loggedViewCount && RectsUnchanged(views, count))
    {
        return;
    }
    g_loggedViewCount = count;

    const auto engineSkip = *reinterpret_cast<const std::int32_t*>(viewFamily + family::kSkipFrameNumberIncrement);
    const auto engineAdd  = *reinterpret_cast<const std::uint8_t*>(viewFamily + family::kAdditionalViewFamily);
    // Logged on each change of the view list (Debug): the split's own lines say when it opens and closes.
    platform::LogDebug(L"game viewport submits {} view(s){}; as built by the engine: +0x70={} +0x74={}", count,
                  count > 1 ? L", rendered as one family each" : L"", engineSkip, engineAdd);

    for (std::int32_t k = 0; views != nullptr && k < count; ++k)
    {
        const auto rect = RectOf(views[k]);
        if (k < kRectLoggedViews)
        {
            g_loggedRects[k] = rect;
        }
        platform::LogDebug(L"  view {}: output rect ({},{})-({},{}), {}x{}", k, rect[0], rect[1], rect[2], rect[3],
                      rect[2] - rect[0], rect[3] - rect[1]);
    }
}

// ---------------------------------------------------------------------------------------------
// Frame time: what rendering each view as its own family costs
// ---------------------------------------------------------------------------------------------
//
// At the end of each of the perf log's 10 s windows (platform/frame_watch.h): the mean frame time over the window's
// frames and the view count. Comparing one player with two gives the cost of rendering each view as its own family.
// Under it, one line per family: how long the game thread spent inside the engine's submission of that family. A
// frame time much longer than those means the time goes elsewhere (render thread or GPU); a family whose submission
// alone takes most of the frame means the engine waits inside it.

constexpr std::size_t kTimedFamilies = 4;  // later families share the last slot

std::size_t   g_familyInFrame = 0;  // which family of this frame the next submission is
double        g_submitSeconds[kTimedFamilies]{};
std::uint32_t g_submitCalls[kTimedFamilies]{};

/// The engine's submission of one family, timed on the game thread.
void TimedEngineSubmit(void* rendererModule, void* canvas, std::byte* viewFamily)
{
    const double start = platform::frame_watch::Now();
    g_engineSubmit(rendererModule, canvas, viewFamily);
    const double end = platform::frame_watch::Now();

    const auto slot = (std::min)(g_familyInFrame++, kTimedFamilies - 1);  // parenthesised: a min macro may be in scope
    g_submitSeconds[slot] += end - start;
    ++g_submitCalls[slot];
}

/// The perf log's window report: the mean frame time and the view count, then one line per family. The per-family
/// times start afresh either way.
void ReportFrameTime(std::uint32_t frames, double seconds, bool log) noexcept
{
    if (log && frames > 0)
    {
        const platform::frame_watch::Job job(L"frame-time log");
        platform::Log(L"frame time {:.2f} ms average over {} frames, {} view(s)", 1000.0 * seconds / frames, frames,
                      g_loggedViewCount);
        for (std::size_t k = 0; k < kTimedFamilies; ++k)
        {
            if (g_submitCalls[k] != 0)
            {
                platform::Log(L"  family {}: {:.2f} ms per frame on the game thread inside the engine's submission",
                              k + 1, 1000.0 * g_submitSeconds[k] / g_submitCalls[k]);
            }
        }
    }
    std::fill(std::begin(g_submitSeconds), std::end(g_submitSeconds), 0.0);
    std::fill(std::begin(g_submitCalls), std::end(g_submitCalls), 0u);
}

// ---------------------------------------------------------------------------------------------
// The vtable replacement
// ---------------------------------------------------------------------------------------------

// Diagnosis only (family-split off): the game viewport's family goes to the engine as it is, both views in one family,
// to measure what one family per view costs. The renderer then breaks the three multi-view rules
// (SPLITSCREEN-REBUILD-PHILOSOPHY.md §3): the bottom view's ME is wrong and, with the cameras apart, the GPU can crash
// (A04B). Use with own-camera off.
std::atomic<bool> g_familySplit{true};

/// Same signature as FRendererModule::BeginRenderingViewFamily. Must not be inlined: the return
/// address it reads is its caller's.
__declspec(noinline) void SubmitViewFamily(void* rendererModule, void* canvas, std::byte* viewFamily)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller != g_drawReturn)
    {
        g_engineSubmit(rendererModule, canvas, viewFamily);  // UMG and other families: untouched
        return;
    }

    LogFamilyOnChange(viewFamily);
    g_familyInFrame = 0;
    if (!g_familySplit.load(std::memory_order_relaxed))
    {
        TimedEngineSubmit(rendererModule, canvas, viewFamily);  // diagnosis: every view in the engine's one family
        return;
    }
    // Between two families: the game's end-of-frame state reset, in stream order (family_boundary.h).
    SubmitOneFamilyPerView(rendererModule, canvas, viewFamily, &TimedEngineSubmit, &QueueFamilyBoundary);
}

}  // namespace

bool InstallViewFamilySubmission(const platform::GameImage& image) noexcept
{
    const auto entry      = image.Rebase(facts::kBeginRenderingViewFamilyEntry, sizeof(void*));
    const auto engineFn   = image.Rebase(facts::kBeginRenderingViewFamily);
    const auto drawReturn = image.Rebase(facts::kDrawSubmitReturn);
    if (!entry || !engineFn || !drawReturn)
    {
        return false;
    }

    // Both are set before the swap, so the replacement never runs without them.
    g_engineSubmit = reinterpret_cast<SubmitViewFamilyFn>(*engineFn);
    g_drawReturn   = *drawReturn;
    if (!g_entry.Replace(*entry, *engineFn, reinterpret_cast<std::uintptr_t>(&SubmitViewFamily)))
    {
        return false;
    }
    platform::frame_watch::AddWindowReport(&ReportFrameTime);
    return true;
}

void SetFamilySplit(bool onePerView) noexcept
{
    g_familySplit.store(onePerView, std::memory_order_relaxed);
    platform::Log(L"view families: {}", onePerView ? L"one family per view"
                                                  : L"DIAGNOSIS: every view in one family (the engine's own multi-view render)");
}

bool FamilySplit() noexcept
{
    return g_familySplit.load(std::memory_order_relaxed);
}

void RemoveViewFamilySubmission() noexcept
{
    if (!g_entry.Restore())
    {
        platform::Log(L"could not restore the renderer's vtable entry (another mod changed it after us); "
                      L"the submission stays installed and the mod stays loaded");
    }
}

#if LSS_DEV

void RegisterViewFamilyCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"family-split",
                       .shape = control::Shape::OnOff,
                       .help  = L"diagnosis: one view family per player (on, at start), or both views in the engine's "
                                L"one family (off; breaks the multi-view rules, so only with own-camera off)",
                       .set   = [](const Value& v) {
                           const bool on = std::get<bool>(v);
                           SetFamilySplit(on);
                           return Outcome{true, on ? L"one view family per player"
                                                   : L"both views in one family (diagnosis)"};
                       },
                       .get = [] { return Value{FamilySplit()}; }});
}

#endif

}  // namespace lss::renderer
