#pragma once

// Each local player's view point: its shape while the screen is split, and which camera player 2's
// half shows.
//
// The mod replaces the ULocalPlayer vtable entry for GetViewPoint (facts/engine.h) and calls
// the engine's function. With one player the result is exactly the engine's. While player 2 is in:
//   - every player's view fills its half: the game's 16:9 constraint is cleared and the FOV rule
//     becomes MaintainXFOV with the split FOV written in (views/view_shape.h, SplitFov), or the
//     stock MajorAxisFOV when the split FOV is switched off; Ctrl+F4 switches the fill off to
//     compare with bars.
//     Each player's own FOV rule is put back when player 2 leaves.
//   - player 2's half shows player 2's own camera from the join; Ctrl+F6 switches it to player 1's (for testing)
//     (and back). Starting on player 1's means a player-2 camera that is not set up yet (it sits
//     at the world origin until it has a character) is never on screen by default.

#include "views/stream_view_math.h"
#include "views/view_shape.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views {

/// Installs the view-point entry. Call once, after every engine fact has been verified.
/// Returns false, changing nothing (logged), if the entry does not hold the engine's function.
bool InstallPlayerViewPoint(const platform::GameImage& image) noexcept;

/// Puts the engine's own function back (at mod uninstall).
void RemovePlayerViewPoint() noexcept;

/// Whether the entry holds the replacement: the filled halves, the camera choice and the streaming views need it.
bool ViewPointInstalled() noexcept;

/// What the view points follow, from the session (mod.cpp fills it each tick, and at a leave before the world
/// changes).
struct SplitFrame
{
    const void*   playerOne  = nullptr;  // player 1's ULocalPlayer
    const void*   playerTwo  = nullptr;  // player 2's; null when there is none
    std::uint32_t joinCount  = 0;        // the session's join count: a new player 2 at the old one's address is new
    bool          splitShown = false;    // player 2 in and the screen split (players/single_view.h)
};

/// Game thread, once per tick after the layout is held (the halves' shape reads it), and at a leave before the
/// world changes. A new player 2 (by address or join count): the FOV rule's offset confirmed by reflection
/// (players/split_settings_access.h; until then the split views fill without changing the FOV rule), the FOV rule
/// of every remaining player the mod changed put back, the new player 2 started on their own camera with their next
/// view point described in the log, and each local player's vtable logged (a subclass would bypass the camera
/// choice). The split's shown state: while it is not shown, player 1's view point is exactly the engine's (the
/// camera choice for player 2's own view still applies). The halves' shape (views::HalfOf): read twice a second
/// while the split is shown and no third view is in; the split FOV is applied only once it is known.
void Follow(const SplitFrame& frame) noexcept;

/// Switches player 2's half between player 1's camera and player 2's own; returns true when it
/// now shows player 2's own (false, and player 1's, when there is no player 2).
bool ToggleOwnCamera() noexcept;

/// True while player 2's half shows player 2's own camera.
bool ShowsOwnCamera() noexcept;

/// Game thread: player 1's or player 2's camera as it was last drawn while player 2 is in (empty before
/// the first split frame, after a change of players, or if the view info was unusable).
std::optional<StreamCamera> RenderedCamera(bool playerTwo) noexcept;

/// The split halves' width/height (views::HalfOf), or 0 until known.
float HalfAspect() noexcept;

/// Whether the split is on screen, as last followed. Any thread.
bool SplitShown() noexcept;

/// Switches whether split views fill their halves (on at start); returns the new state.
bool ToggleFill() noexcept;

/// True while split views fill their halves; false while they keep 16:9 with bars, and always false without the
/// entry (nothing shapes the halves then; the main cameras keep their constraint).
bool FillsHalves() noexcept;

/// The FOV of filled halves (views/view_shape.h, SplitFov): the weight from 0 (keep the
/// single-player framing) to 1 (keep the single-player scale); kDefaultSplitFovWeight at start.
/// Anything outside 0..1 selects the stock MajorAxisFOV rule instead.
void SetSplitFovWeight(float weight) noexcept;

/// The weight in use, or a negative value for the MajorAxisFOV rule.
float SplitFovWeight() noexcept;

/// A player's half width against the game camera's own, as tan(half's horizontal FOV / 2) / tan(camera's / 2) from
/// the last view drawn (1 when no split FOV is written). Any thread.
float SplitWidthRatio(bool isPlayerTwo) noexcept;

/// True if `localPlayer`'s vtable is the engine's ULocalPlayer vtable, i.e. its view point goes
/// through the installed entry. A different vtable (a subclass) would bypass the camera choice.
bool UsesEngineViewPoint(const void* localPlayer) noexcept;

}  // namespace lss::views
