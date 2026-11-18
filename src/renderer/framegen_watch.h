#pragma once

// Diagnosis: DLSS Frame Generation with several views (research/RE-p2-framegen-2.md, RE-p2-framegen-4.md). A dev tool
// (dev/dev_tools.h; a player's build has the empty stubs below).
//
// The game drives Streamline's DLSS-G per view: each view is its own Streamline viewport, set up through function
// pointers the game loaded from sl.interposer.dll and keeps in its own globals. This tool swaps three of them
// (slSetConstants, slFreeResources, slDLSSGSetOptions) for wrappers that forward every call and record per viewport, and
// watches the tags through renderer/frame_generation's own slSetTag wrapper. Per viewport: its options (mode, frames,
// flags, dynamic size and each change of it), its constants (the frame of each call, repeats and skipped frames, the
// camera's position, forward, FOV, aspect, jitter, the resets), each resource tag (type, resource, extent, lifecycle as
// asked and as sent, command list), its frees, and the order the viewports come in. Per frame: any resource tagged for
// two viewports in the same frame and sent valid until present (by present it holds the last one's). A summary goes to
// the log every few seconds while calls flow.
//
// Switches for the live tests:
//   - `fg-off-viewport <id>` forwards that viewport's options with mode off (0: none; -1: every viewport);
//   - `fg-tags-now off|depth|all`: renderer/frame_generation's rule (depth by default) off, or for every frame input.
// The game's own structs are never written: a changed call forwards a copy.
//
// Off at start: `fg-watch on` places the wrappers (once the game has loaded Streamline), `fg-watch off` puts the
// game's pointers back.

#include "platform/dev_switch.h"

namespace lss::renderer::framegen_watch {

#if LSS_DEV

/// Game thread: the summary, every few seconds while the wrappers see calls.
void Tick() noexcept;

/// Game thread: puts the game's pointers back.
void Remove() noexcept;

/// The `fg-watch`, `fg-off-viewport` and `fg-tags-now` commands and readings. May throw (out of memory).
void RegisterCommands();

#else

inline void Tick() noexcept {}
inline void Remove() noexcept {}

#endif

}  // namespace lss::renderer::framegen_watch
