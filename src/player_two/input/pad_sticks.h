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
// engine call, and puts the merged values back afterwards (a Set range of platform/byte_scope.h:
// were the game to write the floats inside the call, its newer values would be kept and noted).
// Nothing inside such a call writes those floats (their only writer runs before the world tick).
// A merged stick that is exactly zero stays zero, which keeps the game's own "not focused" rule.
//
// Two kinds of call are scoped while per-pad sticks are on (from install; Ctrl+F9 switches them,
// for testing):
//   - each player's controller input (AEndPlayerController::ProcessPlayerInput, slot 400) reads its
//     own pad; player 2's also runs as the primary player's, so the camera events it sends reach
//     player 2's camera manager;
//   - player 1's character's move state reads player 1's pad (player_two/input/move_state.h), so does the
//     chocobo player 1 rides, and player 2's member is ticked with player 2's pad
//     (player_two/input/member_movement.h).
// Both act only on the game thread (player_two/generation.h); a call on any other thread is left alone.

#include "platform/byte_scope.h"
#include "platform/dev_switch.h"
#include "player_two/input/stick_select.h"
#include "player_two/stage_counters.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::pad_sticks {

/// Finds the input object and replaces the controller-input slot. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

void Remove() noexcept;

/// Switches per-pad sticks; returns the new state of the switch.
bool Toggle() noexcept;

/// Per-pad input acts now: the switch is on and it is not stood down. Per-pad buttons (player_two/input/pad_buttons.h)
/// follow this too.
bool Enabled() noexcept;

/// The switch alone (Toggle's state), whether or not it is stood down.
bool SwitchedOn() noexcept;

/// Stands per-pad input down while `on` (the buggy or the bronco, player_two/rides/shared_ride.h: every pad drives
/// player 1's ride,
/// the game's own merge) and back after. Logged on change. Game thread.
void SetStoodDown(bool on) noexcept;

/// Diagnosis: which parts of per-pad sticks act, while they are on (bits: 1 player 2's input runs
/// as the primary player's, 2 player 1's move state reads pad 1, 4 player 2's member is ticked, 8
/// player 1's input update (terrain detection), leader-action tick and ridden chocobo read pad 1). All at
/// start. Logged.
inline constexpr unsigned kPartPlayerTwoInput  = 1;
inline constexpr unsigned kPartPlayerOneMove   = 2;
inline constexpr unsigned kPartMemberTick      = 4;
inline constexpr unsigned kPartPlayerOneDetect = 8;
void SetMask(unsigned mask) noexcept;
unsigned Mask() noexcept;
bool PartActs(unsigned part) noexcept;

#if LSS_DEV
/// Logging (the Wheelie watch, a dev tool): pad `controllerId`'s own left and right trigger from the game's per-pad
/// records (the most pressed over the banks the game merges), whatever any scope holds. False when no record could be
/// read. Game thread.
bool PadTriggers(std::int32_t controllerId, float& left, float& right) noexcept;
#endif

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
    platform::ByteStore<1, sizeof(Sticks)> context_;
    bool                                   held_ = false;
};

StageCounters& InputCounters() noexcept;
StageCounters& MoveCounters() noexcept;

#if LSS_DEV
/// The `sticks-mask` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::player_two::pad_sticks
