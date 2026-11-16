#pragma once

// Player 2's or 3's character never fires its own story box ahead of the story, and never loses one
// (research/RE-member-story-boxes.md).
//
// Each character's own body is swept for the level's trigger boxes every tick, whoever controls it (its tracker,
// character +0x9F0). A box limited to some members (its ObjectLayout row's actor group lists the member's tag and not
// the party leader's) stands where that member arrives when the level moves it or when it follows its leader in; in
// single player its trigger fires at the story's moment. A member its player walks ahead reaches such a box early, and
// the level step it starts can play out of order or stall. So the box's enter (FieldStateTrigger_OnColliderBeginOverlap)
// is redirected: for a member its player holds, on no move the level sent, a member-only box that is switched on and
// sends a trigger is not entered now but remembered (the game then records nothing; its leave is safe with no record).
// The member enters it later through the game's own enter with the same three values, the moment a companion would in
// single player:
//   - the level moves it or hands it to its AI (the session's ride along), or writes a move of its own on it, even one
//     its player keeps (session/lead_gate OnLevelMove): in single player that move carries the companion through its
//     boxes;
//   - its player leaves (it follows on its AI);
//   - player 1's character comes within a few metres of the box (world/story_box_rules.h Reaches): a following
//     companion walks in about then.
// While its player is in, never during a cutscene, a menu or the map. If the member is no longer inside the box then,
// the game's leave follows at once, so the box's occupant record stays balanced. The game itself enters boxes later
// this way (its field step replays the leader's enters queued during a load). Party boxes, group-less boxes, boxes
// switched off and every character on its AI are the game's. Info lines: each enter held back, each replay, and each
// remembered enter whose box went away first (its level unloaded): the only way one is dropped.
// Game thread only: the enter runs inside the field tick; Tick runs outside it and outside every player's scope.

#include "world/story_box_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::story_boxes {

/// Finds the enter, its leave and the reads; the redirect is placed by Tick. False (logged) on a failed check: the
/// boxes are then the game's, as before.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The layout object `object` is a member-only story box for `member` (its group lists `member`'s tag and not the party
/// leader's, as the game's send tests) and is switched on with a trigger to send. False when not installed or unread.
/// session/lead_gate asks it of a level walk's way out (OwnBoxAhead). Game thread.
bool ArmedOwnBox(std::uint64_t object, const void* member) noexcept;

/// The current state row (FName bits) of layout object `object`'s trigger slot (attribute 1, through the game's slot
/// lookup): for a story box, the state whose Status names the trigger it sends. 0 when none, unread or not installed.
/// session/lead_gate compares its Status with a walk's exits. Game thread.
std::uint64_t TriggerStateRow(std::uint64_t object) noexcept;

/// The placed shape of the box of layout object `object` that `member` walked into and whose enter waits. False when
/// none waits for it (the character never entered that box) or its shape was not read.
bool HeldShape(const void* member, std::uint64_t object, story_box_rules::Box& out) noexcept;

/// `member` was brought back for the level's walk from `from` to `to` (world/run_catch_up): each of its waiting enters
/// whose box that straight walk passes through waits for the character's own body instead of firing at its AI's hand
/// (story_box_rules Next). Returns how many. Game thread.
int AwaitRun(const void* member, const float from[3], const float to[3]) noexcept;

/// Game thread, each engine tick outside the field tick and every player's scope: places the redirect on the first
/// call, then replays or drops the remembered enters. `splitShown`: the split is on screen (no cutscene, menu or map).
void Tick(bool splitShown) noexcept;

}  // namespace lss::world::story_boxes
