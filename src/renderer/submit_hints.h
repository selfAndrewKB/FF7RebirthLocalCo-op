#pragma once

// Split screen without the per-parallel-pass GPU submissions (RE-perf-submission-path.md §6 option C; measured live
// 2026-10-09: -0.14 ms per frame in split, faster in 4 of 4 pairs).
//
// Each parallel pass the engine dispatches puts an FRHICommandSubmitCommandsHint node into the RHI command list. When
// the RHI thread walks it, the direct context closes its command list and submits it: one ExecuteCommandLists, and each
// submission has a fixed GPU cost (the GPU idles ~5 % of a split frame with work queued). The work recorded before the
// pass is closed and queued ahead of the parallel lists anyway, and goes out with them or with the next flush, so GPU
// order is unchanged. The mod changes one byte of the `lea` that gives the node its vtable: new nodes get the vtable
// 8 bytes further on, whose first slot is an engine `ret 0`, so the node stays in the list and does nothing. Only the
// parallel passes' hints change; the engine's explicit flushes (transitions, fences, DLSS, Present, the end of the
// frame) and the other hint producers do not. Performance work, authorized as a change to game behaviour (the plan's
// performance roadmap). Held while player 2 is in, so one-player play keeps the game's hints; `submit_hints_on.txt` or
// `watch_only.txt` in the mod's folder keeps them always.

namespace lss::renderer {

/// Game thread, after start-up: checks the site and both vtables, and logs what it found. Changes nothing yet.
void InstallSubmitHintSkip() noexcept;

/// True if the site was verified (the control command and the join/leave switch act).
bool SubmitHintSkipInstalled() noexcept;

/// Game thread: parallel passes skip their submit hint while `skip` is true. Acts and logs only on a change.
void SetSubmitHintSkip(bool skip) noexcept;

/// Game thread, on removal: the game's hints back.
void RemoveSubmitHintSkip() noexcept;

}  // namespace lss::renderer
