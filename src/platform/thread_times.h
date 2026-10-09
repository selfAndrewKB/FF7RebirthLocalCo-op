#pragma once

// CPU time per frame of the engine's three frame threads (game, render, RHI), for the perf log.
//
// A frame-time change alone cannot say which thread a performance change helped: the user's PC and the cloud
// PC are bound by different threads, and a profiler capture inflates the RHI thread (RE-perf-cloudpc-costa-
// 2026-10-08.md §5.1). The CPU time is Windows' own accounting (GetThreadTimes); the thread's cycle count over
// the TSC rate of the window is logged beside it as a cross-check, since Microsoft does not guarantee cycles
// to convert to time (on a virtual machine especially). A thread near
// the frame time binds; with none near it, the GPU does. The render and RHI threads are found by the names
// the engine gives them ("RenderThread…", "RHIThread"); the game thread is the caller's.

#include <cstdint>

namespace lss::platform::thread_times {

/// Game thread, at the end of each frame-time window of `frames` frames. Logs one line when `log` is true;
/// either way starts the next window.
void EndWindow(std::uint32_t frames, bool log) noexcept;

}  // namespace lss::platform::thread_times
