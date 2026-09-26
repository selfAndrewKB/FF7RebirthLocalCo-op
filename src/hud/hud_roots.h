#pragma once

// The HUD's root widgets and whose they are (lever L1 of hud/player_two_pass.h).
//
// The HUD roots are the UUserWidgets that sit on the overlay itself (no slot) and belong to the
// game instance. Each carries a player context (UUserWidget+0x248: weak local player, weak world),
// which the marker list and the world-anchored widgets read to choose the controller they project
// with. All roots are player 1's (RE-p2-hud.md P8.10). UUserWidget::SetPlayerContext (stock UE4)
// sets it, and walks the widget's child user widgets; it sends no events.
//
// Roots come and go (the game adds markers as it needs them), so the list is rescanned from
// hud/user_widgets.h, which needs no walk of every UObject. Game thread only.

#include <cstddef>

namespace RC::Unreal {
class UObject;
}

namespace lss::platform {
class GameImage;
}

namespace lss::hud::hud_roots {

/// Checks SetPlayerContext's prologue. False (logged by the caller) if it differs.
bool Install(const platform::GameImage& image) noexcept;

/// The game instance's first two local players, or false while there are fewer. The game instance
/// is found once and then only checked for liveness.
bool LocalPlayers(RC::Unreal::UObject*& one, RC::Unreal::UObject*& two);

/// Refreshes the root list and both players' contexts. False if a context cannot be built yet.
bool Scan(RC::Unreal::UObject* playerOne, RC::Unreal::UObject* playerTwo);

/// Adds widget to the root list at once if it is a root not listed yet (a root the game made since
/// the last scan and that is known now, e.g. an enemy's ability-name label), so this frame's context
/// swap reaches it. Game thread, before SetContexts(1).
void Adopt(RC::Unreal::UObject* widget);

/// Gives player 2's context (1) to every live root shown this frame, or player 1's (0) back to
/// exactly the roots given player 2's. False if the engine raised an exception inside
/// SetPlayerContext.
bool SetContexts(int player) noexcept;

/// How many roots the last scan found.
std::size_t Count() noexcept;

}  // namespace lss::hud::hud_roots
