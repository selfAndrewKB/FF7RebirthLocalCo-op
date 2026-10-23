#pragma once

// Each half's target marks follow its own player's target (RE-p2-hud-targets.md §2.3, §3, R2, R3).
//
// Enemy gauges: which enemies have a name and HP gauge is decided by one collection (the displayed
// target, enemies whose AI asks to always show status, and a sticky list), read by the gauge manager's
// two steps that create and keep entries. Both calls are redirected: the game's collection runs, then
// runs again on the same list with player 2's target fields in place (player_two/lock_on.h's
// TargetScope). It only appends what is missing, so the list is both players' targets together and
// the game makes, keeps and fades player 2's gauge itself. Both sites or neither: one keeps only what
// its list names.
//
// Per view: before each view's HUD updates and pass, the marks that belong only to the other player
// get render opacity 0 (the game's own UWidget::SetRenderOpacity, players/widget_opacity.h) and their
// own opacity back after that pass. Still painted, so UMG keeps ticking them (fades and animations
// advance; a Hidden or Collapsed widget would never tick, as player 2's pass runs with DeltaTime 0):
//   - player 2's pass: player 1's cursor, and gauges only player 1's displayed target brought in;
//   - player 1's pass: player 2's cursor (player_two/lock_on.h, Cursor), and gauges only player 2's
//     displayed target brought in.
// The red ability label rises above an enemy's name and gauge only when that enemy is the locked-on
// target: its lookup considers only the gauge of the bar manager's targeted key, player 1's. Its one
// call is redirected: during player 2's updates the key holds player 2's displayed target for that
// call only, so each player's label makes way for that player's own locked-on gauge.

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::target_views {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// At the start of each frame's overlay render: the frame's cached lookups dropped.
void BeginFrame() noexcept;

/// Before `player`'s HUD updates and overlay pass (0 player 1, 1 player 2): the other player's marks
/// collapsed.
void BeforePass(int player) noexcept;

/// After a pass: every mark hidden for it back to its own opacity.
void AfterPass() noexcept;

/// A lock-on target TWeakPtr {object, controller} and, if it is a part, its owner's main target
/// (EndUI__GetMainTargetOfLockOnTarget); zeros if it is gone.
std::array<std::uintptr_t, 2> TargetAndMain(std::uintptr_t object, std::uintptr_t controller) noexcept;

}  // namespace lss::hud::target_views
