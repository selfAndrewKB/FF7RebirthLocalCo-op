#pragma once

// Each view's target marks follow its own player's target (research/RE-p2-hud-targets.md).
//
// Enemy gauges: which enemies have a name and HP gauge is decided by one collection (the displayed
// target, enemies whose AI asks to always show status, and a sticky list), read by the gauge manager's
// two steps that create and keep entries. Both calls are redirected: the game's collection runs, then
// runs again on the same list once per seat with that seat's target fields in place
// (battle/lock_on.h's TargetScope, one at a time). It only appends what is missing, so the list is
// every player's targets together and the game makes, keeps and fades each seat's gauge itself. Both sites or
// neither: one keeps only what its list names.
//
// Per view: before each view's HUD updates and pass, the marks that belong only to the other player
// get render opacity 0 (the game's own UWidget::SetRenderOpacity, players/widget_opacity.h) and their
// own opacity back after that pass. Still painted, so UMG keeps ticking them (fades and animations
// advance; a Hidden or Collapsed widget would never tick, as a seat's pass runs with DeltaTime 0): each
// view's pass takes out every other player's cursor (player 1's the battle system's, a seat's
// battle/lock_on.h Cursor), and the gauges other players' displayed targets brought in that its own
// player's did not.
// The red ability label rises above an enemy's name and gauge only when that enemy is the locked-on
// target: its lookup considers only the gauge of the bar manager's targeted key, player 1's. Its one
// call is redirected: during a seat's updates the key holds the seat's displayed target for that
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

/// Before the seat's HUD updates and overlay pass (0 player 1's): the other players' marks collapsed.
void BeforePass(int seat) noexcept;

/// After a pass: every mark hidden for it back to its own opacity.
void AfterPass() noexcept;

/// A lock-on target TWeakPtr {object, controller} and, if it is a part, its owner's main target
/// (EndUI__GetMainTargetOfLockOnTarget); zeros if it is gone.
std::array<std::uintptr_t, 2> TargetAndMain(std::uintptr_t object, std::uintptr_t controller) noexcept;

}  // namespace lss::hud::target_views
