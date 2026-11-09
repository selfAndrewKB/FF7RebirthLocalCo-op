#pragma once

// A character player 2 or 3 walked past its own story box is brought back beside player 1's character when the level's
// walk through that box goes to its AI (research/RE-member-story-boxes.md §9, §12).
//
// The lead gate hands a walk whose step moves on at the character's own armed story box to its AI (session/lead_gate
// OwnBoxAhead), so the game's walk carries the character in on the game's clock. A character its player took past that
// box cannot be carried in: the walk from there to its point never passes through the box (story_box_rules RunCrosses),
// so left alone, the box's held-back enter fires at the hand and the step plays where the character stands. In single
// player the game's follow AI keeps a companion near its leader. So, just before the hand, when the walk from where the
// character stands misses its box and the walk from beside player 1's character passes through it:
//   - a navmesh point 1.5 m from player 1's character toward the walk's point, else 35 degrees to either side, else
//     behind it, never within 1 m of another player's character (the game's own query, as its battle gather asks it);
//   - the game's battle-warp move on the character there (a queued warp and a short fade-in), facing the walk's point;
//   - the character's held-back enters this walk passes through wait for its own body (world/story_boxes AwaitRun);
//   - the hand waits for the next check: the character's tracker sweeps the warp's whole path on its next pass, and a
//     box on that line must meet the hold while the character is still its player's (held back, merged with the
//     waiting enter), not the AI's (fired early).
// Nothing when the character never entered the box (no held enter: the walk carries it in from where it stands), is in a
// field action, can already walk through the box, when player 1's character is past it too, or when no navmesh point
// is found (logged; the hand goes on as before). One move per walk. Reading `world.run-catch-up` and switch
// `run-catch-up` (dev builds). Game thread, outside every player's scope.

namespace lss::platform {
class GameImage;
}

namespace lss::world::run_catch_up {

bool Install(const platform::GameImage& image) noexcept;

/// Just before the guide hand of `seat`'s member (session CheckHeldAi): brings it back when the rule above holds. True
/// when moved: the caller hands it at its next check. Game thread.
bool BeforeGuide(int seat, void* member) noexcept;

}  // namespace lss::world::run_catch_up
