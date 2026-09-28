#pragma once

// A diagnostic probe for the main-menu crash (RE-p2-main-menu-crash.md, the plan's main-menu bug).
//
// With player 2 in, opening the pause menu a second time crashes on the RHI thread: a draw's
// pixel-shader resource array holds a view whose descriptor handle is 0, and the driver reads
// address 0. The probe (renderer/null_view_probe_code.h) records each such view and gives its
// element the null descriptor, exactly as the game does for an empty element, so the run goes on
// and the log says which view it was. It changes nothing while every view has a handle.
//
// This is a probe, not the fix: the fix is at whatever leaves the view without a handle.

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

bool InstallNullViewProbe(const platform::GameImage& image) noexcept;
void RemoveNullViewProbe() noexcept;

/// Game thread, each engine tick: logs new hits (the latest view's details).
void PollNullViewProbe(bool playerTwoIn) noexcept;

}  // namespace lss::renderer
