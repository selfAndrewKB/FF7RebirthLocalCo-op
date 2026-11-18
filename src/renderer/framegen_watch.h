#pragma once

// Diagnosis: DLSS Frame Generation with two views (research/RE-p2-framegen-2.md). A dev tool (dev/dev_tools.h; a
// player's build has the empty stubs below).
//
// With frame generation on, split screen shows one player's half black. The game drives Streamline's DLSS-G per view:
// each player's view is its own Streamline viewport, tagged and set up through function pointers the game loaded from
// sl.interposer.dll and keeps in its own globals. This tool swaps four of those pointers for wrappers that forward
// every call unchanged and record, per viewport: its options (mode, frames, flags, dynamic size), each resource tag
// (type, resource, extent, lifecycle, command list), its constants calls, its frees, and the order the viewports come
// in. A summary goes to the log every few seconds while calls flow, with the shared-resource check (the same D3D12
// resource tagged for two viewports).
//
// Two switches change what is forwarded, for the live tests of the research doc:
//   - `fg-off-viewport <id>` forwards that viewport's options with mode off (0: none): does one viewport alone keep the
//     rest of the screen?
//   - `fg-tags-now depth|all|off` forwards the depth tag (or depth, motion vectors, HUD-less colour and UI) with the
//     lifecycle "only valid now", so Streamline copies it at tag time: is a shared input the cause?
// The game's own structs are never written: a changed call forwards a copy.
//
// Off at start: `fg-watch on` places the wrappers (once the game has loaded Streamline), `fg-watch off` puts the
// game's pointers back. Logging only while both switches are off.

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
