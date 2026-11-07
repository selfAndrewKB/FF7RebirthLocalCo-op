#pragma once

// Each local player's view point: its shape while the screen is split, and which camera each seat's view
// shows. Per player (players/seats.h): player 1 and every seat; a mirror view (views/mirror_player.h) is
// none of them and keeps the engine's result.
//
// The mod replaces the ULocalPlayer vtable entry for GetViewPoint (facts/engine.h) and calls
// the engine's function. With one view on screen the result is exactly the engine's. While several views show (a
// seat's own view, or the columns the mirror views fill):
//   - every view fills its column: the game's 16:9 constraint is cleared and the FOV rule becomes MaintainXFOV with
//     the view's own FOV written in (views/split_fov_rules.h, from the view's own shape: its fractions of the
//     viewport, so a column of a wider monitor keeps the camera's vertical FOV and a narrow column the single-player
//     scale), or the stock MajorAxisFOV when the split FOV is switched off; Ctrl+F4 switches the fill off to compare
//     with bars. A mirror view's rule is written without a record (the mod's own player).
//     Each player's own FOV rule is put back when a seat's player leaves. A scene's camera cut writes the
//     game's rule over the mod's and may not put the mod's back; the rule is written again when the split
//     returns after player 1's single view (views/axis_records.h RetakeAll).
//   - a seat's view shows its own camera from the join; Ctrl+F6 switches these views to player 1's (for
//     testing) (and back). Starting on player 1's means a camera that is not set up yet (it sits at the world origin
//     until it has a character) is never on screen by default.
//   - a seat's view shows player 1's camera instead while views/column_source.h says so (the game takes
//     the whole screen or a shared ride while columns are held: a held column is never black; or that seat's player
//     alone waits for a party member: with columns held, or on one monitor during the short grace before its player
//     leaves, session/session_rules.h WaitOutside), as a mirror view does, and draws player 1's HUD (the seat's pass
//     does not run then).

#include "platform/dev_switch.h"
#include "players/seats.h"
#include "views/stream_view_math.h"
#include "views/view_shape.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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
    const void* playerOne = nullptr;  // player 1's ULocalPlayer
    std::array<const void*, players::kMaxPlayers>   seats{};  // each seat's ULocalPlayer, null while out (index 0 unused)
    std::array<std::uint32_t, players::kMaxPlayers> joins{};  // each seat's join count: a new player at an old address
    bool splitShown  = false;  // a seat in and its own view on screen (views/column_source.h)
    std::array<bool, players::kMaxPlayers> onPlayerOne{};  // each seat's view shows player 1's camera (index 0 unused)
    bool viewsSplit  = false;  // more than one view on screen: the HUD is laid out for player 1's column
    int  columns     = 1;      // local players listed, one column each (views/columns.h)
};

/// Game thread, once per tick after the layout is held (the halves' shape reads it), and at a leave before the
/// world changes. A seat's new player (by address or join count): the FOV rule's offset confirmed by reflection
/// (players/split_settings_access.h; until then the split views fill without changing the FOV rule), the FOV rule
/// of every remaining player the mod changed put back, the new player started on their own camera with their next
/// view point described in the log, and each local player's vtable logged (a subclass would bypass the camera
/// choice). The split's shown state: while one view is on screen, player 1's view point is exactly the engine's (the
/// camera choice for a seat's own view still applies). The viewport's size (views::ViewportSizeWatch): read twice a
/// second while several views show; the split FOV is applied only once it is known.
void Follow(const SplitFrame& frame) noexcept;

/// Switches every seat's view between player 1's camera and its own, together; returns true when they
/// now show their own (false, and player 1's, when no seat is in).
bool ToggleOwnCamera() noexcept;

/// True while the seats' views are set to show their own cameras (the first one in's setting; player 1's
/// still shows in a view views/column_source.h puts on player 1's picture).
bool ShowsOwnCamera() noexcept;

/// Game thread: `seat`'s camera (0 player 1's) as it was last drawn while a seat is in (empty before
/// the first split frame, after a change of players, or if the view info was unusable).
std::optional<StreamCamera> RenderedCamera(int seat) noexcept;

/// `seat`'s view's width over its height (0 player 1's) as last shaped, or 0 until known (one view on screen, the
/// viewport's size unread). Any thread.
float ViewAspect(int seat) noexcept;

/// Whether a seat's own view is on screen, as last followed: the one place it is kept (the overlay render
/// reads it here for the seats' HUD passes). Any thread.
bool SplitShown() noexcept;

/// Whether `seat`'s view shows player 1's camera (views/column_source.h), as last followed: their HUD pass is skipped
/// then, so their view draws player 1's HUD as a mirror view does (hud/passes/column_hud_images.h). False for player 1
/// and for a seat out. Any thread.
bool OnPlayerOne(int seat) noexcept;

/// Whether more than one view is on screen (two or three columns, not the engine's single view), as last followed:
/// the HUD is laid out for player 1's column then (the overlay layout, the field-action icons' base). With no
/// column held beyond the players it is SplitShown. Any thread.
bool ViewsSplit() noexcept;

/// Switches whether split views fill their halves (on at start); returns the new state.
bool ToggleFill() noexcept;

/// True while split views fill their halves; false while they keep 16:9 with bars, and always false without the
/// entry (nothing shapes the halves then; the main cameras keep their constraint).
bool FillsHalves() noexcept;

/// The FOV of a filled view wider than the camera's image (views/split_fov_rules.h, SplitFov): the weight from 0
/// (keep the single-player framing) to 1 (keep the single-player scale); the rule's default at start. Anything
/// outside 0..1 selects the stock MajorAxisFOV rule instead.
void SetSplitFovWeight(float weight) noexcept;

/// The weight in use, or a negative value for the MajorAxisFOV rule.
float SplitFovWeight() noexcept;

/// The weight in use as the option's word ("0.50", views/view_shape.h SplitFovWordOf); empty under the MajorAxisFOV
/// rule (no word of the option's).
std::wstring SplitFovWord();

#if LSS_DEV
/// The `split-fov.horizontal` command and reading, from the option's row (platform/settings.h), and
/// `split-fov.native`, the stock MajorAxisFOV rule in its place for a comparison. May throw (out of memory).
void RegisterViewPointCommands();
#endif

/// `seat`'s view width (0 player 1's) against the game camera's own, as tan(view's horizontal FOV / 2) /
/// tan(camera's / 2) from the last view drawn (1 when no split FOV is written). Any thread.
float SplitWidthRatio(int seat) noexcept;

/// True if `localPlayer`'s vtable is the engine's ULocalPlayer vtable, i.e. its view point goes
/// through the installed entry. A different vtable (a subclass) would bypass the camera choice.
bool UsesEngineViewPoint(const void* localPlayer) noexcept;

}  // namespace lss::views
