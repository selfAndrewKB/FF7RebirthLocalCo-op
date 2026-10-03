#pragma once

// The game's NPC population follows both players (plan §3 item 7, the user's choice of 2026-10-02;
// RE-p2-charapop-spawn-gate.md §2).
//
// CharaPop keeps a budget of characters spawned, scored by the distance of each one's spawn point from one
// centre: the party leader, or the override centre event scripts set with CharaPop_SetOverrideCenter. Far
// from that centre a character is despawned (states 9 → 10 → 11), whatever its detail level, so the
// townspeople near player 2 vanish while player 1 is far. There is one centre and no path for a second.
//
// While the split is on screen and player 2 plays, the mod sets the game's override centre to the
// midpoint of the two players' characters with the game's own setter, and widens the population's
// distance parameters (DistanceMaxValue and DistanceHardLimitValue, loaded once from the game's param
// table) by half the players' separation, so the midpoint population reaches both. The loaded values are
// put back, and the override cleared, when the split closes, player 2 stops playing, or the switch turns
// it off. A centre the mod did not set (an event's) is left alone, with the parameters at their loaded
// values, until it clears. Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::charapop_centre {

/// Finds the game's setter and the parameters; logged either way. False if a check failed.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether the split is on screen and player 2 plays a member.
void Tick(bool splitShown, bool playerTwoPlays) noexcept;

/// The comparison switch (Ctrl+F10, `npc-centre on|off`): on at start. Logged.
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

bool Installed() noexcept;

/// Puts the parameters back and clears the mod's centre (at mod uninstall; on the game thread only, else logged).
void Release() noexcept;

}  // namespace lss::player_two::charapop_centre
