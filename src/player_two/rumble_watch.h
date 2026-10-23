#pragma once

// Logging only: where each player controller's force feedback goes, pad by pad, frame by frame
// (RE-p2-rumble.md §6; plan, "Per-pad rumble"). A dev tool (dev/dev_tools.h installs it; a player's build has the
// empty stubs below): the only view of the per-pad device writes.
//
// The stock path is per controller: each PC's ProcessForceFeedbackAndHaptics sends its own values with its own
// ControllerId through the PC's vt+0xC90 send. Square's platform layer then turns ControllerId 0 into the "last active
// device" code (app+0x1E0, XInput pad k = -2-3k), and the XInput device, given such a code, plays that pad and writes
// zero to every other pad. Which of the two players' writes reaches pad 2 last in a frame decides whether player 2's
// pad rumbles. Two vtable entries are wrapped, read-only (each passes straight to the game's function):
//   - AEndPlayerController's send (vt+0xC90): which PC, the id it sends, the device code, its values, its effects;
//   - the XInput device's SetChannelValues (slot 6): every pad write, in order (the negative code's per-pad writes
//     come back through the same slot).
// A frame is logged when any value in it is non-zero, or when its shape (who sent, which pads were written, zero or
// not) differs from the last frame logged; a status line every 10 s while player 2 is in. Game thread.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::rumble_watch {

#if LSS_DEV

/// Wraps the two entries (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with player 2 out).
void Remove() noexcept;

/// The controller's active force-feedback effects (APlayerController::ActiveForceFeedbackEffects.Num), or -1.
std::int32_t EffectCount(const void* controller) noexcept;

#else

inline void         Install(const platform::GameImage&) noexcept {}
inline std::int32_t EffectCount(const void*) noexcept { return -1; }

#endif

}  // namespace lss::player_two::rumble_watch
