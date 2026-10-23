#pragma once

// Logs the game's field interactions (FieldActionMgr) around player 2's pickups (RE-p2-field-interact.md §6, §10). A
// dev tool: dev/dev_tools.h installs it; a player's build has the stub below. It changes nothing: its own two hooks
// pass the game's calls on, and the pass's three hooks (player_two/world/field_pickup.h) tell it what passed through
// them.
//
// FieldActionMgr keeps every field prompt as a point. Each frame its update picks one target per button slot around
// one probe, the party leader; a press runs the slot's key, and a WorldItem key marks the item taken and grants it to
// the shared inventory. Player 2's pass (field_pickup) runs the same code once more as player 2. Logged:
//   - each point registration (AddPoint, its own hook: key, owner, presets, kind, the registrar, the leader type),
//     and counts per registrar for a summary;
//   - after each selection update and player 2's pass: the slot block on change (target keys, rows, latches,
//     pending) and, while player 2 is in, every 0.5 s:
//       - a census of the points within 20 m of either player, logged on change (key, owner, presets,
//         slot, kind, flags, whether it is a WorldItem, the distance to each player);
//       - a dry run of the game's own candidate query around player 2's member into a mod-owned array
//         (writes nothing else), its keys logged on change;
//   - each slot execution (its own hook: the slot's key, the leader, the primary controller, whether a WorldItem took
//     it);
//   - each WorldItem mark (key, result, which caller, whether in player 2's pass) and grant (key, the primary
//     controller the rumble goes to, each controller's force-feedback effects around it).
// Pad 2's FieldAction presses are logged by player_two/pad_buttons.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::interact_watch {

#if LSS_DEV

/// Installs the two pass-through hooks (each only if its code is what the research read) and the pass's observer.
/// After field_pickup. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with player 2 out).
void Remove() noexcept;

#else

inline void Install(const platform::GameImage&) noexcept {}

#endif

}  // namespace lss::player_two::interact_watch
