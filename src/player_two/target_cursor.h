#pragma once

// Player 2's own target cursor (RE-p2-hud-targets.md §2, R2).
//
// The game's target cursor is one widget entry in the target-cursor manager (EndUISystem[22]), made,
// moved, styled and released by the displayed-target update for player 1's displayed target. The
// manager holds any number of entries (another game system keeps its own), and its per-frame tick
// places each with the widget's own player controller, which the mod's HUD pass already re-runs for
// player 2's view. So player 2 gets a second entry for player 2's displayed target, driven with the
// game's own create, move, style, flag and release functions and the game's own tests (the battle
// list's block, the target's "show a cursor" answer, the game flags), with player 2's member where
// the game passes its leader. The widget is taken from each create, never kept across a release
// (released widgets go to one shared reuse slot). Each half shows only its own player's cursor
// (hud/target_views.h). Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::target_cursor {

bool Install(const platform::GameImage& image) noexcept;

/// Once per frame in battle, after player 2's displayed target is settled: player 2's cursor made,
/// moved or released as the game does for player 1's. `target` is player 2's displayed target (a
/// TWeakPtr {object, reference controller}).
void Update(std::uintptr_t system, void* member, std::uintptr_t targetObject, std::uintptr_t targetController,
            bool locked) noexcept;

/// Player 2's cursor released (battle over, player 2 out, or the cursor switched off).
void Release() noexcept;

/// Player 2's cursor widget while it lives, else null.
void* Widget() noexcept;

}  // namespace lss::player_two::target_cursor
