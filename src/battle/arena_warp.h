#pragma once

// Player 2's or 3's character brought into the fight on a double click of the left stick: a character can be shut
// outside a fight (a crawlspace the game closes while it lasts), and nothing in Rebirth brings a battle member in during
// a fight (research/RE-arena-warp.md).
//
// The game has no in-battle recovery warp; what it does to a member far from a fight is the battle start's gather. In a
// fight that gather places the member with the game's own navmesh query around the field leader (3 m, else 1 m, at
// 70 and 50 degrees to either side) and moves it with its battle warp (a queued warp, a ground snap, the control holder
// back to its first state, a fade-in). The mod makes those two calls for the asking player's member, aimed off player
// 1's facing (no enemy is needed), when that player clicks L3 twice within 0.4 s in a battle (world/nav_strand reads the
// raw key per pad, as for the field's ask; L3 is the game's Dash, which a fight reads held: two short holds). The warp
// runs on the next engine tick, outside every player's scope, with the game's own gates:
//   - in a battle (the encounter's state 3; not while player 1 escapes, state 4);
//   - this battle's start reached the game's own gather with the member among those it gathers (battle/battle_warps:
//     a fight whose data gathers no one, or keeps this member out, gets no warp from the mod either);
//   - the member alive (not knocked out, not petrified: the game's battle-switch test);
//   - no event holding the party (the leader switch's own counters);
// and the mod's: in no field action and on no ride of its own. The game's battle-area test is read for the log, not
// as a gate: a character can be cut off from the fight while that test still counts it inside, and neither the
// navmesh query nor the battle warp tests the area. The trigger is the mod's: the game has no in-battle ask for a
// battle member (the one recreation, written down in the plan and the research). Logged: each ask's answer (Info),
// with the area test's reading, and the member's area test half a second after a warp. Outside a battle the same
// double click is the field's ask (world/party_warp). Game thread.
//
// Player 1's ask goes the other way: in a fight a player 2 or 3 started (the enemies noticed them, battle/encounter_watch),
// player 1's double click or the period key moves player 1's character beside that player's, with the same placement
// and battle warp around that character. The game's gather would have moved player 1 there too (a fight anchored on
// another character gathers the field leader) and the battle-warps setting kept it in place. The gates: the same
// game's ones, then two of the mod's, as the warp resets the character's control to idle: player 1's command menu
// closed, and player 1's character idle (an attack, ability, guard, dodge or hit reaction is waited out for up to 1.5 s,
// then the ask is refused); not when already beside.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::arena_warp {

bool Install(const platform::GameImage& image) noexcept;

/// A battle is on: the encounter's state 3, or 4 while player 1 escapes. Game thread.
bool InBattle() noexcept;

/// `seat`'s player asked (L3 twice, the period key, the console; `how` a string literal; 0 for player 1) during a battle:
/// made at the next Tick (player 1's within 1.5 s).
void Ask(int seat, const wchar_t* how) noexcept;

/// Each engine tick, game thread, outside every player's scope: makes the asked warps; forgets the gather when the
/// battle ends.
void Tick() noexcept;

}  // namespace lss::battle::arena_warp
