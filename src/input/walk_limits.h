#pragma once

// Each seat's walk and run limits come from its own surroundings (players/seats.h; RE-p2-walk-mode.md §4.2, §4.3).
//
// A character's speed is capped by move-type layers on "the" primary player controller. Two of them depend on
// where the character stands:
//
// - Layer 8 (InDoor rooms) is written by the controller's UEndPlayerExistNavAreaComponent, which the game creates
//   for local player 0's controller only. Each seat's controller gets one from the game's own creation function, run
//   once inside that seat's scopes::primary_controller::Scope; the component's tick already runs as the seat's
//   (camera/location_rows.h), so the game's InDoor rooms then reach the seat's controller and camera.
// - Layer 7 (location walk zones such as a town's) is decided by the global location manager for one character, the
//   party leader, and written through its listener to the primary controller. When a seat's player takes a member,
//   the game's party switch copies the leader's layer 7 onto that controller, and nothing ever clears it there.
//   The manager has no per-player test and also drives BGM, names and flags, so it cannot run for a seat; this is
//   a documented recreation of its bookkeeping only (plan, RE-p2-walk-mode.md §4.3). Each tick the mod tests each
//   seat's member against the game's move-type volumes with the game's own containment test and rules (strictly inside
//   to enter, the volume's UntouchMargin to stay, bForce and bDisabled honoured, leaves before enters), keeps the
//   game's refcounted move-type stack for that seat, and when the seat's controller's layer 7 differs from the stack's
//   top, calls the game's own listener method inside the seat's scopes::primary_controller::Scope to write it.
// Per seat: the inside bits, the stack and the retry and log state; the seats run in turn. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::input::walk_limits {

bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the session's tick (a take's copy is overwritten in the same frame): every built seat in turn.
void Tick() noexcept;

}  // namespace lss::input::walk_limits
