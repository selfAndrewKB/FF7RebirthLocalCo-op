#pragma once

// Each pad's sticks drive only its own player.
//
// Square's input layer merges every pad into one stick value per frame: before the world ticks, it
// copies whichever pad's left stick and right stick has the largest magnitude into four floats of
// its input object (+0x1210: LX, LY, RX, RY), and every stick read during the tick returns those.
// The per-pad values are kept alongside, one 24-byte record {LX, LY, RX, RY, LT, RT} per pad
// (indexed by controller id) in each device bank (+0x11D0, 16 bytes per bank header).
//
// A StickScope replaces the four merged floats with one pad's own record for the length of one
// engine call, and puts the merged values back afterwards. Nothing inside such a call writes those
// floats (their only writer runs before the world tick). A merged stick that is exactly zero stays
// zero, which keeps the game's own "not focused" rule.
//
// Two kinds of call are scoped, while per-pad sticks are switched on (Ctrl+F9, off at start):
//   - each player's controller input (AEndPlayerController::ProcessPlayerInput, slot 400) reads its
//     own pad; player 2's also runs as the primary player's, so the camera events it sends reach
//     player 2's camera manager;
//   - player 1's character's move state reads player 1's pad (player_two/move_state.h), and player
//     2's member is ticked with player 2's pad (player_two/member_movement.h).
// Both act only on the game thread (player_two/generation.h); a call on any other thread is left alone.

#include "player_two/stage_counters.h"
#include "player_two/stick_select.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::pad_sticks {

/// Finds the input object and replaces the controller-input slot. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

void Remove() noexcept;

/// Switches per-pad sticks; returns the new state.
bool Toggle() noexcept;

bool Enabled() noexcept;

/// The controller id of the local player a controller belongs to, or -1.
std::int32_t ControllerIdOf(const void* controller) noexcept;

/// Replaces pad `controllerId`'s sticks with `sticks` in every StickScope for that pad, whatever the
/// pad itself reports and whether or not the game window has focus; null puts the pad back. Used by
/// control scripts (control/command_channel.h). Game thread only.
void SetStickOverride(std::int32_t controllerId, const Sticks* sticks) noexcept;

class StickScope
{
public:
    explicit StickScope(std::int32_t controllerId) noexcept;
    ~StickScope();
    StickScope(const StickScope&)            = delete;
    StickScope& operator=(const StickScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    float saved_[4]{};
    bool  held_ = false;
};

StageCounters& InputCounters() noexcept;
StageCounters& MoveCounters() noexcept;

}  // namespace lss::player_two::pad_sticks
