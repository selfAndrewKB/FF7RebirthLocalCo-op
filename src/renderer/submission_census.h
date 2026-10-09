#pragma once

// Diagnosis: a census of the game's GPU submissions (RE-perf-submission-census.md; the parent doc's §7).
//
// The GPU idles ~5 % of a split frame with work queued, and removing submissions is what has paid (the gap recorder,
// the parallel passes' submit hints). This counts every ExecuteCommandLists the engine makes, per frame: which code
// asked (by the two return addresses above the command-list manager's submit), on which queue, how many lists, and
// how much recorded work they carry (the context's six counters), so the next merges can be chosen from data.
//   - the command-list managers' shared submit slot (vtable [1]) goes through a counting wrapper;
//   - four of the engine's submit-hint producers (SE's RDG hint passes, Niagara, async-compute dispatch, ME's async
//     end) get tagged copies of the hint vtable, so hint submissions split by producer. The parallel passes' site is
//     never retargeted (renderer/submit_hints owns it); an untagged hint on the direct queue is that one;
//   - the contexts' signal-without-flush slot (0x108) counts signals issued while unsubmitted work exists: the one
//     hazard of the submit-hint skip.
// Off at start: `census on` (control) or `census.txt` in the mod's folder at launch. The summary goes into the perf
// log's 10 s window (Info, with `perf-log on`). Logging only; nothing the game does changes.

namespace lss::renderer {

/// Game thread: installs the hooks (logs what it found). True if the submit wrapper is in.
bool InstallSubmissionCensus() noexcept;

/// Game thread: `census.txt` in the mod's folder installs it at start-up.
void InstallSubmissionCensusIfAsked() noexcept;

/// Game thread: puts every hook back (the vtable copies stay: queued nodes may still use them).
void RemoveSubmissionCensus() noexcept;

bool SubmissionCensusInstalled() noexcept;

/// Game thread, from the perf log's window: one summary of the window (per frame), then a new window.
void LogSubmissionCensus() noexcept;

}  // namespace lss::renderer
