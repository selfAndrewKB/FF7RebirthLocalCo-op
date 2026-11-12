#pragma once

// Between the players' view families, the game's own end-of-frame state reset, in RHI stream order
// (research/RE-p2-main-menu-crash.md).
//
// Why: the D3D12 state cache's SRV-array records (SE code) only grow, and only
// FD3D12CommandContext::ClearState empties them, once per frame in RHIEndFrame. Each scene render
// drains the pending-delete list, so with more than one family a later family's drain can destroy a
// texture an earlier family bound; that later family's next flush of the slot then copies the destroyed
// view (handle 0) and the driver reads address 0. Solo the end-of-frame reset always comes first.
//
// What: between submission k and k+1 the family boundary runs exactly what RHIEndFrame runs
// between frames on the default direct context: ClearState, then RHISubmitCommandsHint (ClearState
// alone is unsafe mid-list: it resets cached values without their re-send flags; the submit's
// reset re-dirties them, as every game caller pairs it). Each family then starts from the same
// state the first family of a frame starts from.
//
// How, all through the game's own functions and objects:
//   1. game thread, between two submissions: a render-thread task queued with the game's
//      TGraphTask SetupPrereqs/QueueTask, the same queue BeginRenderingViewFamily
//      puts its own render commands on, so it runs after family k's and before family k+1's;
//   2. render thread: a 16-byte node appended to the immediate RHI command list, exactly as the game
//      appends its own EndFrame and SubmitCommandsHint nodes (or run at once while the list is being
//      executed, the game's own EnqueueLambda rule);
//   3. RHI thread, in stream order: the game's ClearState and RHISubmitCommandsHint on the list's
//      context, only if it is the default direct context.
// The exception to "only the game's code": the task's and the node's two-entry vtables, and the
// node append (family_boundary_list.h), which the game inlines into every command and never
// exposes as a function. There is no game command that clears state mid-frame; this is the least
// that issues one (the plan records it as a documented exception).

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

bool InstallFamilyBoundary(const platform::GameImage& image) noexcept;

/// Game thread, between two families' submissions: queues the boundary reset. Does nothing if
/// not installed or after a fault.
void QueueFamilyBoundary() noexcept;

/// Game thread, each engine tick: logs what the boundary did (every 30 s while it runs).
void PollFamilyBoundary() noexcept;

}  // namespace lss::renderer
