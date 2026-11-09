#pragma once

// The battle's rates: enemy damage, enemy HP, boss HP, the enemies' stagger gauge, EXP and AP, each a multiplier from
// 0.1 to 10.0 set from the Co-op page, the console or the settings file (platform/settings.h). Each drives the game's
// own computation (research/RE-battle-rates.md, RE-battle-rate-sites.md):
//   - HP and the stagger gauge: the game computes an enemy's from its tables in one function, at the enemy's battle
//     start (its component's vtable slot 0xD8, replaced to name the component) and at each phase change (named by the
//     phase change's own stagger-share read just before). The game's values are scaled before the game sets them: the
//     boss rate for an enemy whose base row marks it the fight's leader (the game's own boss notion), the enemy rate
//     otherwise; a party-side unit, and an enemy whose data fixes its HP, stay as the game makes them. From the next
//     fight (a phase change mid-fight takes the rate in force).
//   - Enemy damage to the party: the game's own per-hit difficulty step (an enemy-side attacker, a party-side victim, not
//     a heal, its own hit kinds) multiplies by an Easy or Hard row; on Normal (and Dynamic, which the game reads as
//     Normal) it has none. The step's difficulty read answers Easy for Normal while the rate is not 1.0, and its row
//     read answers the row (1.0 on Normal) times the rate: the game multiplies, rounds and keeps a hit at least 1.
//   - EXP: the game sets each enemy's EXP when it enters a fight; its base value read answers times the rate, before
//     the game's own level and bonus factors. From the next fight.
//   - AP: the game's own bonus (after the story) at the award: its gate answers open while the rate is not 1.0, and its
//     bonus row answers the game's percent (100 while the bonus is off) times the rate. The award's rounding and each
//     member's share stay the game's.
// Logging: Info at install and at each change of a rate; Debug at each scaled enemy, hit and award (only while its
// rate is not 1.0). Hits and stats may run on any thread (the rates are atomics; the per-call state is the thread's).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_rates {

/// Reads the settings and registers the six options, then finds the game's functions and places the redirects (each
/// lever whole or not at all: one that cannot be placed is logged and its rates stay the game's).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::battle_rates
