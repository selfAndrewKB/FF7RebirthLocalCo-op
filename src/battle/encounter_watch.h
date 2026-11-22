#pragma once

// The encounter watch: what the field encounter does, logged; and with the option enemies-notice on, field enemies
// noticing players 2 and 3 (research RE-field-battle-start-seats.md).
//
// The game watches one character for every field enemy: its encounter tick reads the party leader once per tick and
// tests that one character's sight, territory, gauge and reach against each enemy; the manager's one state (field,
// caution, battle, escaping) follows it, a fight starts in one call, and the battle-in gather anchors on the field
// leader. The watch passes each of those three calls through unchanged and logs around them: each change of the
// manager's state with every player's nearest field enemy and whether they stand in its territory (the game's own test);
// which player a "nearest human inside a territory" rule would watch; a player 2 or 3 near a field enemy; each enemy's
// force bytes and perception states as they change; the preemptive prompt; the enemies a fight starts with and who
// stands in its area after; the gather's anchor. The console's encounter-watch reads the enemies and each player's
// distance.
//
// Enemies-notice (off by default): the tick's read answers the character of the player who stands nearest a field
// enemy inside its territory (the game's own territory test; kept until another player is clearly nearer), chosen
// only while the encounter is in its field state, so caution, the fight and the escape keep their player: the fight
// starts around them and ends when they leave its area, as the game's escape test decides for the one it watches. The
// fight's gather stands around that player (its anchor read), and the battle-warps setting decides who it moves
// (battle/battle_warps.h: off keeps player 1's character too). Setup, the HUD and the battle's end stay player 1's.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::encounter_watch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The player 2 or 3 the fight now starting, or running, noticed (its gather stands around them); 0 for player 1's.
/// Game thread.
int NoticedPlayer() noexcept;

}  // namespace lss::battle::encounter_watch
