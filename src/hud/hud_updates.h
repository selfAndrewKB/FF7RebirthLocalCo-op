#pragma once

// The HUD's view-dependent updates that can run more than once a frame (RE-p2-hud.md pass 9 and
// pass 12), for hud/passes/player_two_pass.h: the lists at EndUISystem+0x98, +0xB0, +0x150 (markers),
// +0x188, +0x170, +0x180, then the leader-target icon at +0x140, in MasterHUD_Tick's order, and the
// compass holder's update. All are Square Enix End code. The whole MasterHUD_Tick cannot be re-run
// (per-call counters, queues, sounds); these can, with the game mode's HUD time step at 0.
//
// Every call is guarded (platform/guarded.h). Game thread only.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::hud_updates {

/// Checks every update's prologue and finds EndUISystem. False if any differs.
bool Install(const platform::GameImage& image) noexcept;

/// Player 2's run of the marker list with each view's own arrow timers (hud/field/marker_timers.h): the
/// game mode's HUD time step is `realStep` for that one list (`timeStep` is the field), 0 again after.
struct OwnArrowTimers
{
    float* timeStep = nullptr;
    float  realStep = 0.0f;
};

/// Runs the lists and the leader-target icon (if `lists`) and the compass (if `compass`) with a
/// DeltaTime of 0 (the marker list as `arrows` says, if given). False if one raised an exception;
/// the rest are then skipped.
bool Run(bool lists, bool compass, const OwnArrowTimers* arrows = nullptr) noexcept;

/// Each update's cost in ms per frame since the last call, for a log line; resets the counts.
std::wstring TakeTimings(unsigned frames);

}  // namespace lss::hud::hud_updates
