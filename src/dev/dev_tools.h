#pragma once

// The development tools, behind one switch: the control-script channel (control/), the HUD trace
// (hud/hud_trace.h), the position probe (hud/position_probe.h) and the dump commands. They exist to
// observe the game and to drive it without keys; a player needs none of them.
//
// They are built only with the CMake option LSS_DEV_TOOLS (on by default). Without it, the calls
// below are empty, and neither the tools' code nor their engine redirects are in the DLL.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two {
class Session;
}

namespace lss::players {
class SplitLayout;
}

namespace lss::dev {

#if defined(LSS_DEV_TOOLS) && LSS_DEV_TOOLS

/// Installs the logging redirects (idle until a command starts them) and opens the control channel.
void Install(const platform::GameImage& image, player_two::Session& session, players::SplitLayout& layout,
             bool viewPointInstalled) noexcept;
void Remove() noexcept;

/// Game thread, once per engine tick: a control script's next step, before the session's tick.
void TickBeforeSession() noexcept;

/// Game thread, once per engine tick, after the session's tick: drops stick holds of ended joins.
void TickAfterSession() noexcept;

/// Game thread, around the overlay render (hud/root_overlay_render.h).
void BeforeOverlayRender() noexcept;
void AfterOverlayRender() noexcept;

#else

inline void Install(const platform::GameImage&, player_two::Session&, players::SplitLayout&, bool) noexcept {}
inline void Remove() noexcept {}
inline void TickBeforeSession() noexcept {}
inline void TickAfterSession() noexcept {}
inline void BeforeOverlayRender() noexcept {}
inline void AfterOverlayRender() noexcept {}

#endif

}  // namespace lss::dev
