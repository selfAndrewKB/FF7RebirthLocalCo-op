#pragma once

// One streaming-feedback hand-in per frame for the game's scenery ("ME") streaming, as with one player
// (RE-p2-distant-lod-swap.md §9).
//
// ME keeps one set of detail requests per scenery chunk. Each scene render processes the last readback
// and copies its own requests for the next one. The game renders one family per frame and so alternates
// process and copy; with one family per player (renderer/one_family_per_view.h) both families do both,
// and their copies overwrite each other: a shadow caster outside player 1's view flips between detail
// levels several times a second. The game has no switch for one family only (its MassiveEnvironment
// show flag turns off all ME work, picture included), so while player 2's family renders (the scene
// renderer's copy of bAdditionalViewFamily, renderer+0x84, set by the mod on later families only) this
// skips three game calls: the readback processing in the visibility gate, the main pass's copy and the
// hidden-component pass. Player 2's family still draws ME, its shadows and its slave dispatches;
// player 2's detail requests reach player 1's family through player 2's slave view
// (views/stream_views.h). A skip of game calls, not a game lever: recorded in the plan as such.
//
// Each skipped and passed call is counted per family and summarised in the log.
// The switch (`stream-feedback p1|each`) is on (p1) from install.

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

bool InstallMeFeedbackOnce(const platform::GameImage& image) noexcept;
void RemoveMeFeedbackOnce() noexcept;  // only while one family renders per frame (player 2 out)
bool MeFeedbackOnceInstalled() noexcept;

/// True: player 1's family alone hands in requests. False: every family runs the game's calls.
void SetMeFeedbackOnce(bool playerOneOnly) noexcept;
bool MeFeedbackOnceOn() noexcept;

/// Game thread, each engine tick: writes the per-family counts line when its period is up.
void PollMeFeedbackOnce() noexcept;

}  // namespace lss::renderer
