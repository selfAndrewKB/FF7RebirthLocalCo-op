#pragma once

// Each seat's command controller gets the battle's command values through the game's own setters
// (RE-p2-command-menu-levers.md §1, §3; IDA passes S-CMD9, S-CMD10).
//
// The game writes these into its one command controller only (EndUISystem[23]); they are battle-wide,
// so each seat's controller takes the same values:
//   - commands enabled (+0x5E2: every row greyed while 0) and locked (+0x5E3): BattleSystem_Tick calls
//     two one-line setters on the battle system's dirty lock byte; their one call site each is redirected;
//   - the per-category event and tutorial locks (+0x2D8[k]): SetCommandCategoryEnabled, called from the
//     event on/off table and scripted battles (12 call sites, 4 crossing a code block), so its entry is
//     taken;
//   - four bytes the event on/off table's dispatcher stores itself (+0x77C, +0x77F the key-guide gate,
//     +0x780, +0x782 actions while the menu is hidden); its entry is taken.
// Each setter runs as the game called it, then once more with the UI system's controller slot naming
// a seat's controller. The dispatcher runs once (it also drives battle-wide effects: music, encounters,
// HUD masks); the bytes it changed on the game's controller are stored on each seat's right after it
// returns. Nothing is replayed before a seat's controller exists or off the game thread (logged).
//
// A written exception (RE §1.3, §3.3, §8): the game pushes these values only when they change, and a
// seat's controller misses every push made before it follows (it is built later, and its creation Setup
// resets +0x782). Sync() copies them once after each join Setup.
//
// Per seat: every built seat's controller follows each setter in seat order (the seat's controller is
// battle/command_menu's); a seat's join Sync copies to that seat's only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_locks {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// After a join Setup of `seat`'s controller `controller` (not one made with the game's): the values the game pushed
/// before that controller followed its pushes, copied from the game's controller.
void Sync(int seat, void* controller) noexcept;

}  // namespace lss::battle::command_locks
