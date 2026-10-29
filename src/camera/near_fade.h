#pragma once

// No seat's character (players/seats.h) is ever faded by the game's proximity fades
// (research/RE-p2-camera-near-fade.md).
//
// The fades are on the character, so a fade meant for player 1's view hid player 2's own character in both halves:
// - Near-camera fade: every frame the game gathers the characters within 5 m of player 1's camera and fades out (hidden
//   reason bit 0x10, over 0.2 s) those closer than their near-fade distance. The gather's result is filtered: every
//   seat's member is taken out of the list, and the game's own diff then never fades it (or fades it back in). The
//   camera managers' immediate kick (a subject that opted out of the pass, faded at once by its own camera) skips it too.
// - Occluder fade: player 1's camera's traces toward its pivot admit every party member's mesh; a hit on a seat's
//   character (or anything attached to it) resolves to no actor, so the game skips the hit.
// - Overlap fade (§9): climbing walls, spider walls and one-shot moves fade a character to 0.3 (reason bit 9) while
//   another party member overlaps it, unless the game's player test says it is the player's; the test answers yes for
//   a seat's member.
// Every other character, and everything else in the fades, stays the game's. The seat comes from the character the
// game hands over. Per seat: the logged states. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::camera::near_fade {

/// Replaces the four calls (each checked; logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, per frame: a Debug line when a seat's character's fade state changes (reasons, masks, opacity
/// target).
void Tick() noexcept;

}  // namespace lss::camera::near_fade
