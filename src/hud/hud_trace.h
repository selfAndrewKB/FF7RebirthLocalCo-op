#pragma once

// Logging only: how the HUD's content is made each frame, and for which player (RE-p2-hud.md
// pass 8, P8.5 items 3 and 6-9; call sites in P8.9). Everything here passes the game's calls
// through unchanged.
//
// `hud-trace <frames>` logs one line per frame for that many frames. Each line gives the frame
// number and the order of these calls, redirected at their call sites (platform/call_redirect.h):
//   T  MasterHUD_Tick(&EndUISystem, DeltaTime), from the game mode's tick (0x1423EF4A1);
//   M  EndUI__PlaceMarkerWidgetAtWorld_Clamped(pc, marker, ...), in the marker list's tick
//      (0x140C8D620); the pc is the marker widget's own player (its PlayerContext);
//   P  EndUI__ProjectWorldToRootOverlay(pc, ...), the compass and quest icons (four sites in
//      AreaMapIcon_FrameUpdate and its helper); the pc is the world's first controller;
//   R  the overlay render (hud/root_overlay_render.h, which calls TraceBeforeRender and
//      TraceAfterRender around it); each frame's line is logged there.
// It also counts M and P by the player whose controller was passed.
//
// A dev tool, installed on the first `hud-trace` (control/mod_target.cpp), not at every dev start: its seven call
// redirects sit on the HUD's hot path.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

bool InstallHudTrace(const platform::GameImage& image) noexcept;
void RemoveHudTrace() noexcept;
bool HudTraceInstalled() noexcept;

/// The most frames one trace logs.
inline constexpr std::int32_t kMaxHudTraceFrames = 30;

/// Logs the next `frames` frames (clamped to 1..kMaxHudTraceFrames).
void StartHudTrace(std::int32_t frames) noexcept;

/// Game thread, around the overlay render (hud/root_overlay_render.h); stubs without the dev tools.
#if LSS_DEV
void TraceBeforeRender() noexcept;
void TraceAfterRender() noexcept;
#else
inline void TraceBeforeRender() noexcept {}
inline void TraceAfterRender() noexcept {}
#endif

}  // namespace lss::hud
