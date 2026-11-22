#pragma once

// The encounter watch: what the field encounter does, logged, for enemies noticing players 2 and 3 (research
// RE-field-battle-start-seats.md). Changes nothing.
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

namespace lss::platform {
class GameImage;
}

namespace lss::battle::encounter_watch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::encounter_watch
