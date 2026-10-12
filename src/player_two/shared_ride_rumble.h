#pragma once

// Logging only: what the buggy's and the bronco's rumble would meet on player 2's pad (RE-p2-rumble-owner-chocobo.md
// §9.6; the user asked for it before deciding whether both pads rumble on a shared ride, 2026-10-12).
//
// Every vehicle rumble goes to the game's one list, which player 1's leader-action component plays on player 1's
// controller. Playing it on player 2's pad too would mean a copy in player 2's list (or a second apply on player 2's
// controller). The game's per-channel apply (VibrationChannel_ApplyToPC758) plays only when the slot table of the
// controller's force-feedback component's manager does not already name the effect (the component is embedded at
// `PC+0x758`, its manager pointer at `+8`; slots `+0x90`/`+0x98`, 64 bytes each, effect name `+0x20`; both builds). If
// both controllers' components share one manager, a copy of an effect player 1 plays on the same channel would be
// skipped.
//
// Logged (game thread):
//   - Info, once player 2 has a controller (again when either component or manager changes): each controller's
//     component and manager, and whether the manager is one for both;
//   - Info at each shared ride's start and end; Debug, during it, each change of a slot's effect name in player 1's
//     manager (and player 2's when separate); at the end, the effects seen.

namespace lss::player_two::shared_ride_rumble {

/// Once per engine tick, after shared_ride::Tick (`sharedRide`: player 1 rides the buggy or the bronco now).
void Tick(bool sharedRide) noexcept;

}  // namespace lss::player_two::shared_ride_rumble
