#pragma once

// Logs the game's field interactions (FieldActionMgr) around the seats' pickups (RE-p2-field-interact.md §6, §10). A
// dev tool: dev/dev_tools.h installs it; a player's build has the stub below. It changes nothing: its own two hooks
// pass the game's calls on, and the pass's three hooks (world/field_pickup.h) tell it what passed through
// them.
//
// FieldActionMgr keeps every field prompt as a point. Each frame its update picks one target per button slot around
// one probe, the party leader; a press runs the slot's key, and a WorldItem key marks the item taken and grants it to
// the shared inventory. A seat's pass (field_pickup) runs the same code once more as the seat's player. Logged:
//   - each point registration (AddPoint, its own hook: key, owner, presets, kind, the registrar, the leader type),
//     and counts per registrar for a summary;
//   - after each selection update and the seats' passes: the slot block on change (target keys, rows, latches,
//     pending) and, while a seat's player holds a member, every 0.5 s:
//       - a census of the points within 20 m of any player, logged on change (key, owner, presets,
//         slot, kind, flags, whether it is a WorldItem, the distance to each player);
//       - a dry run of the game's own candidate query around each seat's member into a mod-owned array
//         (writes nothing else), its keys logged on change;
//   - each slot execution (its own hook: the slot's key, the leader, the primary controller, whether a WorldItem took
//     it);
//   - each WorldItem mark (key, result, which caller, which seat's pass if any) and grant (key, the primary
//     controller the rumble goes to, each controller's force-feedback effects around it).
// A seat's pad's FieldAction presses are logged by input/pad_buttons.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::world::interact_watch {

#if LSS_DEV

/// Installs the two pass-through hooks (each only if its code is what the research read) and the pass's observer.
/// After field_pickup. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out).
void Remove() noexcept;

#else

inline void Install(const platform::GameImage&) noexcept {}

#endif

}  // namespace lss::world::interact_watch
