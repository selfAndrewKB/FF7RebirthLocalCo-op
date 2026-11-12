#pragma once

// One streaming-feedback hand-in per frame for the game's scenery ("ME") streaming, as with one player
// (research/RE-p2-distant-lod-swap.md).
//
// ME keeps one set of detail requests per scenery chunk. Each scene render processes the last readback
// and copies its own requests for the next one. The game renders one family per frame and so alternates
// process and copy; with one family per player (renderer/one_family_per_view.h) every family does both,
// and their copies overwrite each other: a shadow caster outside player 1's view flips between detail
// levels several times a second. The game has no switch for one family only (its MassiveEnvironment
// show flag turns off all ME work, picture included), so while an additional family renders (the scene
// renderer's copy of bAdditionalViewFamily, renderer+0x84, set by the mod on later families only) this
// skips four game calls: the readback processing in the visibility gate, the main pass's copy, the
// hidden-component pass, and (performance, research/RE-perf-family-duplication.md) phase 8's per-chunk mip-request
// dispatches for the slave frustums, whose marks only the skipped copy reads; and (RE-perf-me-shadows.md Phase 3,
// option D) the shadow pass's streaming-feedback writes, through machine code. An additional family still draws ME and
// its shadows; its player's detail requests reach player 1's family through that player's slave view
// (views/stream_views.h). A skip of game calls, not a game lever: recorded in the plan as such.
//
// Each skipped and passed call is counted per family and summarised in the log.
// The switch (`stream-feedback p1|each`) is on (p1) from install.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::renderer {

bool InstallMeFeedbackOnce(const platform::GameImage& image) noexcept;
void RemoveMeFeedbackOnce() noexcept;  // only while one family renders per frame (every seat out)
bool MeFeedbackOnceInstalled() noexcept;

/// True: player 1's family alone hands in requests. False: every family runs the game's calls.
void SetMeFeedbackOnce(bool playerOneOnly) noexcept;
bool MeFeedbackOnceOn() noexcept;

/// Diagnosis (feedback-mask): which skips act while player 1's family alone hands in requests: 1 the readback
/// processing, 2 the main pass's copy, 4 the hidden-component pass, 8 phase 8's mip-request dispatches, 16 the shadow
/// pass's streaming-feedback writes (renderer/me_shadow_feedback_code.h). All at start.
void SetMeFeedbackMask(std::uint32_t mask) noexcept;
std::uint32_t MeFeedbackMask() noexcept;

/// Game thread, each engine tick: writes the per-family counts line when its period is up.
void PollMeFeedbackOnce() noexcept;

#if LSS_DEV
/// The `stream-feedback` and `feedback-mask` commands and readings. May throw (out of memory).
void RegisterMeFeedbackCommands();
#endif

}  // namespace lss::renderer
