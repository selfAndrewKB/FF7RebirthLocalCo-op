#include "renderer/view_family_submission.h"

#include "platform/engine_addresses.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "renderer/one_family_per_view.h"
#include "renderer/scene_view_family.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include <algorithm>
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
    platform::Log(L"game viewport submits {} view(s){}; as built by the engine: +0x70={} +0x74={}", count,
                  count > 1 ? L", rendered as one family each" : L"", engineSkip, engineAdd);

    for (std::int32_t k = 0; views != nullptr && k < count; ++k)
    {
        const auto rect = RectOf(views[k]);
        if (k < kRectLoggedViews)
        {
            g_loggedRects[k] = rect;
        }
        platform::Log(L"  view {}: output rect ({},{})-({},{}), {}x{}", k, rect[0], rect[1], rect[2], rect[3],
                      rect[2] - rect[0], rect[3] - rect[1]);
    }
}

// ---------------------------------------------------------------------------------------------
// Frame time: what rendering each view as its own family costs
// ---------------------------------------------------------------------------------------------
//
// One line every 10 s: the mean interval between the game viewport's submissions, and the view
// count. Comparing one player with two gives the cost of rendering each view as its own family.
// Under it, one line per family: how long the game thread spent inside the engine's submission
// of that family. A frame time much longer than those means the time goes elsewhere (render
// thread or GPU); a family whose submission alone takes most of the frame means the engine
// waits inside it.

constexpr double      kFrameTimeReportSeconds = 10.0;
constexpr std::size_t kTimedFamilies          = 4;  // later families share the last slot

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_lastSubmit{};
LARGE_INTEGER g_windowStart{};
std::uint32_t g_framesInWindow = 0;

std::size_t   g_familyInFrame = 0;  // which family of this frame the next submission is
std::int64_t  g_submitTicks[kTimedFamilies]{};
std::uint32_t g_submitCalls[kTimedFamilies]{};

double TicksToMilliseconds(std::int64_t ticks) noexcept
{
    return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_frequency.QuadPart);
}

/// The engine's submission of one family, timed on the game thread.
void TimedEngineSubmit(void* rendererModule, void* canvas, std::byte* viewFamily)
{
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    g_engineSubmit(rendererModule, canvas, viewFamily);
    QueryPerformanceCounter(&end);

    const auto slot = (std::min)(g_familyInFrame++, kTimedFamilies - 1);  // parenthesised: Windows.h's min macro
    g_submitTicks[slot] += end.QuadPart - start.QuadPart;
    ++g_submitCalls[slot];
}

void ReportSubmissionTimes() noexcept
{
    for (std::size_t k = 0; k < kTimedFamilies; ++k)
    {
        if (g_submitCalls[k] != 0)
        {
            platform::Log(L"  family {}: {:.2f} ms per frame on the game thread inside the engine's submission",
                          k + 1, TicksToMilliseconds(g_submitTicks[k]) / g_submitCalls[k]);
        }
        g_submitTicks[k] = 0;
        g_submitCalls[k] = 0;
    }
}

void RecordFrameTime(std::int32_t viewCount) noexcept
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_lastSubmit.QuadPart == 0)
    {
        QueryPerformanceFrequency(&g_frequency);
        g_windowStart = now;
    }
    g_lastSubmit = now;
    ++g_framesInWindow;

    const double windowSeconds =
        static_cast<double>(now.QuadPart - g_windowStart.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    if (windowSeconds < kFrameTimeReportSeconds)
    {
        return;
    }
    if (platform::frame_watch::Detailed())  // the perf-log switch
    {
        const platform::frame_watch::Job job(L"frame-time log");
        platform::Log(L"frame time {:.2f} ms average over {} frames, {} view(s)",
                      1000.0 * windowSeconds / g_framesInWindow, g_framesInWindow, viewCount);
        ReportSubmissionTimes();
    }
    else
    {
        std::fill(std::begin(g_submitTicks), std::end(g_submitTicks), 0);
        std::fill(std::begin(g_submitCalls), std::end(g_submitCalls), 0u);
    }
    g_windowStart    = now;
    g_framesInWindow = 0;
}

// ---------------------------------------------------------------------------------------------
// The vtable replacement
// ---------------------------------------------------------------------------------------------

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
    RecordFrameTime(g_loggedViewCount);
    g_familyInFrame = 0;
    SubmitOneFamilyPerView(rendererModule, canvas, viewFamily, &TimedEngineSubmit);
}

}  // namespace

bool InstallViewFamilySubmission(const platform::GameImage& image) noexcept
{
    const auto entry      = image.Rebase(engine::kBeginRenderingViewFamilyEntry, sizeof(void*));
    const auto engineFn   = image.Rebase(engine::kBeginRenderingViewFamily);
    const auto drawReturn = image.Rebase(engine::kDrawSubmitReturn);
    if (!entry || !engineFn || !drawReturn)
    {
        return false;
    }

    // Both are set before the swap, so the replacement never runs without them.
    g_engineSubmit = reinterpret_cast<SubmitViewFamilyFn>(*engineFn);
    g_drawReturn   = *drawReturn;
    return g_entry.Replace(*entry, *engineFn, reinterpret_cast<std::uintptr_t>(&SubmitViewFamily));
}

void RemoveViewFamilySubmission() noexcept
{
    if (!g_entry.Restore())
    {
        platform::Log(L"could not restore the renderer's vtable entry (another mod changed it after us); "
                      L"the submission stays installed and the mod stays loaded");
    }
}

}  // namespace lss::renderer
