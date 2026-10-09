#pragma once

// The GPU's async compute queue on for the session (RE-perf-me-shadows.md, Phase 2, option A).
//
// The engine records compute work on a second queue only when GSupportsEfficientAsyncCompute is set. The device
// creation sets it once, from r.D3D12.AllowAsyncCompute (a read-only setting Square Enix added, 0 in this PC build),
// parallel RHI execution and a vendor rule (AMD; Intel with a condition; NVIDIA with per-primitive shading rates and
// viewport indexing). With it on, Square Enix's async path for the scenery (ME) main-pass culling can be chosen by
// console: r.MassiveEnvironment.UsingAsyncComputeForMainPass 1. The mod writes 1 to the setting from its
// constructor, before the game creates its D3D12 device (renderer/before_device.h); the vendor rule still decides.
// Performance work, authorized as a change to game behaviour (the plan's performance roadmap).
// Off unless `async_compute_on.txt` is in the mod's folder (`watch_only.txt` still wins): on the user's RTX the switch
// alone changed nothing (84.5 against 84.3 fps) and the async main pass cost 1.5 ms of GPU time (2026-10-08), so it
// is kept only to measure other GPUs (the cloud L4).

namespace lss::renderer {

/// From the mod's constructor, before the engine creates its D3D12 device. Logs what it did and why.
void ApplyAsyncComputeOn() noexcept;

/// After the engine is up: logs the setting and whether the device turned async compute on.
void ReportAsyncCompute() noexcept;

}  // namespace lss::renderer
