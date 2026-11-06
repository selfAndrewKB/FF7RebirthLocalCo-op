#pragma once

// Logging only (Debug): every player's traversal camera's collision, frame by frame while a traversal module holds
// slot 15 (RE-p2-traversal-camera-collision.md §1.1): the frames the sweep pulled the camera in, with the climbing
// wall's hip framing off and on, then the hand-over to the follow camera after it left; a line per camera when it
// ends. A dev tool: traversal_camera.cpp installs and ticks it, through the empty stubs below in a player's build.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::traversal_camera::collision {

#if LSS_DEV

/// Keeps the image and finds the climbing camera module's vtable: true when found (the climbing wall's camera is
/// watched too; else the seats' ladders' only).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, per frame: every player's slot-15 module; with every seat's player out the watch is reset.
void Tick(bool active) noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void Tick(bool) noexcept {}

#endif

}  // namespace lss::traversal::traversal_camera::collision
