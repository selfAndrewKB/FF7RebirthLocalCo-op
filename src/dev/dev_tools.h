#pragma once

// The development tools, behind one switch: the console (control/), the HUD trace (hud/hud_trace.h),
// the position probe (hud/position_probe.h), the dump commands, and the watches that only log: the rumble, ride,
// Wheelie, framing, character-detail and hand-over watches, the menu focus, the tag counts, the label relayouts and
// the GPU submission census. They exist to observe the game and to drive it without keys; a player needs none of them.
//
// They are built only with the CMake option LSS_DEV_TOOLS (on by default). Without it, the calls below are empty, and
// neither the tools' code nor their engine redirects are in the DLL. A tool a shipped module still calls (the
// session's hand-over notes, the minimap's relayout scopes, the tag counts, the census summary, the HUD trace's
// render calls) keeps empty stubs in its own header under the same switch (platform/dev_switch.h); the rest is
// reached from here alone.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two {
class Session;
}

namespace lss::dev {

#if LSS_DEV

/// Installs the watches and opens the console (the HUD trace and probe install on their first command).
void Install(const platform::GameImage& image, player_two::Session& session) noexcept;
void Remove() noexcept;

/// Opens the console on the commands registered so far: no engine redirects (the watch-only start uses it,
/// with the console's commands alone, to be quit).
void InstallControl(player_two::Session& session) noexcept;

/// Game thread, once per engine tick: a control script's next step, before the session's tick.
void TickBeforeSession() noexcept;

/// Game thread, once per engine tick, after the session's tick: drops stick holds of ended joins.
void TickAfterSession() noexcept;

/// Game thread, once per engine tick, after the levers the watches observe have ticked: the watches' ticks.
void TickWatches(bool splitShown) noexcept;

#else

inline void Install(const platform::GameImage&, player_two::Session&) noexcept {}
inline void Remove() noexcept {}
inline void InstallControl(player_two::Session&) noexcept {}
inline void TickBeforeSession() noexcept {}
inline void TickAfterSession() noexcept {}
inline void TickWatches(bool) noexcept {}

#endif

}  // namespace lss::dev
