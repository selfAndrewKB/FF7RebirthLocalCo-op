#pragma once

// The game's NPC population follows both players: each NPC is scored from the nearer player (the user's choice of
// 2026-10-18, "approach B", replacing the midpoint of 2026-10-02; RE-p2-charapop-two-centres.md,
// RE-p2-charapop-spawn-gate.md §2).
//
// CharaPop keeps a budget of characters spawned (128 live), ranked by a score whose distance part is measured from one
// centre: the party leader, or the override centre event scripts set with CharaPop_SetOverrideCenter. Far from that
// centre a character is despawned (states 9 → 10 → 11), so the townspeople near player 2 vanished while player 1 was
// far. The game has one centre and no path for a second.
//
// While the split is on screen and player 2 plays, the mod's replacement of the distance score
// (CharaPopEntry_EvalSpawnScore_ByCenter, an entry redirect) runs the game's own score as is, then once more with
// player 2's member as the override centre (the game's own setter, cleared again at once: the resting zero vector),
// and returns the higher of the two: the nearer player's. Everything after the score (sort, budget, despawn, the
// visible budget, load order) follows it unchanged, so each player gets the single-player crowd around them, the game's
// own distances apply from each, and no NPC is spawned for the empty space between them. The score's copy of its old
// transform (+0x80..+0x10F) is put back after the second call. An event's centre (on/off 70 or event command 0x7E;
// the mod never leaves its own set, so a set override is always an event's), a cleared leader centre and externally
// managed entries keep the game's single score. Nothing persistent is written. Game thread only (the score runs only
// there, §3).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::charapop_centre {

/// Finds the game's setter and places the score's redirect; logged either way. False if a check failed.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether the split is on screen and player 2 plays a member.
void Tick(bool splitShown, bool playerTwoPlays) noexcept;

/// The comparison switch (Ctrl+F10, `npc-centre on|off`): on at start. Logged.
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

bool Installed() noexcept;

/// Puts the score's entry back (at mod uninstall; nothing else was written).
void Release() noexcept;

}  // namespace lss::player_two::charapop_centre
