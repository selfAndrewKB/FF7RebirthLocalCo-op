#pragma once

// The D3D12 residency manager off for the session (research/RE-perf-submission-path.md, option A).
//
// Every GPU submission on the RHI thread goes through Microsoft's D3DX12 residency manager: two
// QueryVideoMemoryInfo calls, a residency set, a lock, a worker wake-up, and a GPU wait and signal around the
// real ExecuteCommandLists. Split screen doubles the submissions, and the RHI thread binds the frame. The
// engine's switch, D3D12.ResidencyManagement, is read once, when the D3D12 device is created: if it is 0, the
// device clears the gate byte that every residency call tests. This game ignores the setting in the player's
// Engine.ini (tested live), so the mod clears the gate itself, from its constructor, while the
// exe's delay-import slot for D3D12CreateDevice is still unresolved: the game has created no device, so no
// resource is tracked yet. (d3d12.dll itself is already loaded by then, by something else; seen live.) The
// device only ever clears the gate, so a 0 written early stays 0. Performance work, authorized as a change
// to game behaviour (the plan's performance roadmap). `residency_on.txt` in the mod's folder keeps the
// game's residency management, and so does `watch_only.txt` (the unmodified-game baseline).

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

/// From the mod's constructor, before the engine creates its D3D12 device. Logs what it did and why.
void ApplyResidencyOff() noexcept;

/// After the engine is up: logs the gate's value, which shows whether residency management stayed off.
void ReportResidency(const platform::GameImage& image) noexcept;

}  // namespace lss::renderer
