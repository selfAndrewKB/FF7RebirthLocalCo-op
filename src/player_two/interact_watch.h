#pragma once

// Logs the game's field interactions (FieldActionMgr) for player 2's pickups (read-only;
// RE-p2-field-interact.md §6, §10).
//
// FieldActionMgr keeps every field prompt as a point. Each frame its update picks one target per button
// slot around one probe, the party leader; a press runs the slot's key, and a WorldItem key marks the item
// taken and grants it to the shared inventory. All of it runs in the game mode's tick, outside every
// player-2 window, so nothing near player 2 is ever offered. Before a second pass "as player 2" is built,
// this logs what the game does, without changing it:
//   - each point registration (AddPoint: key, owner, presets, kind, the registrar, the leader type),
//     in full up to a budget, then counts per registrar;
//   - after each selection update (its call in the game mode's tick is redirected): the slot block on
//     change (target keys, rows, latches, pending) and, while player 2 is in, every 0.5 s:
//       - a census of the points within 20 m of either player, logged on change (key, owner, presets,
//         slot, kind, flags, whether it is a WorldItem, the distance to each player);
//       - a dry run of the game's own candidate query around player 2's member into a mod-owned array
//         (writes nothing else), its keys logged on change;
//   - each slot execution (the slot's key, the leader, the primary controller, whether a WorldItem took it);
//   - each WorldItem mark (MarkAcquiredByKey: key, result, which caller) and grant (AcquireEntry: key, the
//     primary controller the rumble goes to).
// Pad 2's FieldAction presses are logged by player_two/pad_buttons.

#include "platform/image.h"

namespace lss::player_two::interact_watch {

/// Installs the five pass-through hooks, each only if its code is what the research read (logged).
void Install(const platform::GameImage& image) noexcept;

}  // namespace lss::player_two::interact_watch
