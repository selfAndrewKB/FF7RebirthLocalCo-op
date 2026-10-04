#pragma once

// Player 2 picks up collectibles and materials with the game's own field prompts (RE-p2-field-interact.md
// §4.1, §12; plan, "Player 2 triggers field prompts").
//
// FieldActionMgr keeps every field prompt as a point and runs one selection per frame around one probe, the
// party leader, with one slot block: the candidates, the visibility-trace handles, eight button slots with
// their rows, records and latches. Nothing in it is per player. So right after the game's own update
// (player 1's; its call in the game mode's tick is redirected by player_two/interact_watch), the same game
// code runs once more as player 2:
//   - inside player 2's scopes: pad 2's sticks and buttons, player 2's controller as the primary one, player
//     2's member as the leader (and its type). Every gate and the probe then resolve to player 2 (§12.7);
//   - with player 2's copy of the manager's per-player ranges swapped in (§12.9; player 1's put back after),
//     and of each point's carried target bits (+0x104 bits 1 and 2), kept by point key;
//   - with the game's own "point disabled" bit (+0x104 bit 4, which the candidate query skips) set on every
//     point that is neither one of WorldItemMgr's items nor one of TreasurePointMgr's chests (§13; the chest
//     listener's own test), and cleared again after: player 2 is offered collectibles, materials and chests only;
//     talk, doors, vending machines, scripts and menus stay player 1's. A chest opened by player 2 runs the game's
//     treasure state on player 2's member (the listener asks for the leader, which the scope makes player 2's);
//   - the input step's FieldAction part in the game's order (its gate, the slot poll into the mod's own
//     buffers, the latch, the press/execute tick), then the selection update;
//   - with player 2's controller first in the world's controller list for each visibility request the pass
//     makes: the request's screen test projects the point through "the first local player" (§12.13), so the
//     offer follows player 2's own screen, not player 1's;
//   - with each point's marker widget (+0x78) swapped too, on every live point, player 2's kept by point key
//     (§12.15): the game's own marker code makes, drives and sweeps player 2's own widget in the pass, so the
//     two players no longer write one widget's state in turn. Player 2's widgets the game would not destroy
//     (point gone between passes, field exit, player 2 leaving) go through its own destroy call.
// Selection, traces, rows, markers, the look-at, the press and the grant are all the game's. The one global
// byte the press tick writes (root+0x210 -> +8) is put back after. The game's field enter/exit resets run
// on player 2's copy too, and its field-exit clear runs on it when player 2 leaves.
//
// Logged: the scopes or gates that stopped a pass (counted), player 2's slot targets and marker widgets on
// change, each widget the mod destroys, a status line once a minute. Game thread only.

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::field_pickup {

/// Finds the game's functions (each checked) and installs the field enter/exit redirects. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Player 2's pass, right after the game's own selection update of `mgr` (the update's redirect).
void AfterUpdate(void* mgr, float dt) noexcept;

/// Player 2's copy of the slot rows (FWeakObjectPtr values, 0 for none), for hud/prompt_rows: the rows
/// player 2's pass made. All 0 while player 2 has no copy.
using Weaks = std::array<std::uint64_t, 8>;
void RowWeaks(Weaks& out) noexcept;

/// True while player 2's pass runs (game thread).
bool InPass() noexcept;

/// A WorldItem key player 2's pass marked taken (player_two/interact_watch's mark hook, inside the pass).
void NoteMarked(std::uint64_t key) noexcept;

/// Whether `key` was marked by player 2's pass (forgotten once asked): the grant then runs with player 2's
/// controller as the primary one, so the item-get rumble plays on pad 2 (the only player read in the grant,
/// RE-p2-field-interact.md §12.12 item 5).
bool TakeMarked(std::uint64_t key) noexcept;

/// Whose point marker widget is (a UFieldTargetWidget at a point's +0x78), as of player 2's last pass: 0
/// player 1's, 1 player 2's, -1 neither (another owner's marker, or player 2 has no copy). For
/// hud/field_markers, which shows each half only its own player's.
int MarkerOwner(const void* widget) noexcept;

}  // namespace lss::player_two::field_pickup
