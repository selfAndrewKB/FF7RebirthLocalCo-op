#pragma once

// The game's memory, the graphics card's, and the system's commit headroom, in the log.
//
// The engine's out-of-memory fatal error ("Ran out of memory allocating N bytes") ends the process when an
// allocation from the OS fails, and it leaves no crash report. A thread of its own samples four times a
// second, so a stalled game or render thread cannot hide the last seconds:
//   - the game's private commit and its peak, and its working set;
//   - the system's commit used, limit and free (RAM plus page file);
//   - the game's use of the graphics card's own memory and the budget Windows gives it (DXGI), since
//     going over that budget moves resources into system memory, which is charged to commit.
// A line every 60 s shows the trend. A change of 0.5 GB in the game's commit or card memory since the last
// line, or the card going over or back under its budget, is logged at the sample (at most once a second), so
// a sudden climb before a crash shows how fast and how far. Under 4 GB of free commit, a line every 10 s.

namespace lss::platform::memory_watch {

/// Starts the sampling thread once (later calls do nothing). It pins the DLL first, then runs until the
/// process ends; it is never joined, so nothing waits on it at unload.
void Start() noexcept;

}  // namespace lss::platform::memory_watch
