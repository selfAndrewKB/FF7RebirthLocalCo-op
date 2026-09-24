#include "renderer/view_family_submission.h"

#include "platform/engine_addresses.h"
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

#include <cstdint>

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
// Each time the number of views changes (a player joins or leaves), one block: the view count,
// the value the engine itself put in +0x70 (the spike's first question: SPLITSCREEN-ARCHITECTURE
// §5), and each view's output rectangle, which confirms view k belongs to player k (player 1 on
// top). Nothing is logged on frames where nothing changed.

std::int32_t g_loggedViewCount = -1;

/// The family's view list: an array of `const FSceneView*`.
const std::byte* const* Views(const std::byte* viewFamily) noexcept
{
    return *reinterpret_cast<const std::byte* const* const*>(viewFamily + family::kViewsData);
}

void LogFamilyOnChange(const std::byte* viewFamily) noexcept
{
    const auto count = *reinterpret_cast<const std::int32_t*>(viewFamily + family::kViewsCount);
    if (count == g_loggedViewCount)
    {
        return;
    }
    g_loggedViewCount = count;

    const auto engineSkip = *reinterpret_cast<const std::int32_t*>(viewFamily + family::kSkipFrameNumberIncrement);
    const auto engineAdd  = *reinterpret_cast<const std::uint8_t*>(viewFamily + family::kAdditionalViewFamily);
    platform::Log(L"game viewport submits {} view(s){}; as built by the engine: +0x70={} +0x74={}", count,
                  count > 1 ? L", rendered as one family each" : L"", engineSkip, engineAdd);

    const auto views = Views(viewFamily);
    for (std::int32_t k = 0; views != nullptr && k < count; ++k)
    {
        const auto rect = reinterpret_cast<const std::int32_t*>(views[k] + view::kUnscaledViewRect);
        platform::Log(L"  view {}: output rect ({},{})-({},{})", k, rect[0], rect[1], rect[2], rect[3]);
    }
}

// ---------------------------------------------------------------------------------------------
// Frame time, for the spike's cost measurement (SPLITSCREEN-ARCHITECTURE §5)
// ---------------------------------------------------------------------------------------------
//
// One line every 10 s: the mean interval between the game viewport's submissions, and the view
// count. Comparing one player with two gives the cost of rendering each view as its own family.

constexpr double kFrameTimeReportSeconds = 10.0;

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_lastSubmit{};
LARGE_INTEGER g_windowStart{};
std::uint32_t g_framesInWindow = 0;

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
    platform::Log(L"frame time {:.2f} ms average over {} frames, {} view(s)",
                  1000.0 * windowSeconds / g_framesInWindow, g_framesInWindow, viewCount);
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
    SubmitOneFamilyPerView(rendererModule, canvas, viewFamily, g_engineSubmit);
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
