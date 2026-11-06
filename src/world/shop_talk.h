#pragma once

// A further player uses a shopkeeper's shop with the game's own field prompt and shop (RE-p2-shopkeeper-talk.md;
// the user, 2026-10-18: "extend shop keeper interaction to P2 as well, similar to the vending machine").
//
// A shopkeeper is a CharaPop ObjectActor NPC with an ordinary field-action point (key = the CharaPop key, e.g.
// oba2100_ShopWeapon01). Pressed, the game's DispatchKey fires the NPC's state trigger (StateTrigger_Request, no leader
// read); the NPC's next state then runs, on later frames and outside the seat's pass, an optional greeting and a
// "show menu" action (type 43; kind 1 FEndShowShopMenu, kind 7 FEndShowSkillTreeMenu), the same shop command vending
// machines use.
//   - Admit: a seat's field pass (world/field_pickup) leaves a point open when its key is a shopkeeper's, read
//     from the game's own data with the game's accessors (§1.2, RE-statechange-row-layout.md: the GS object's talk state,
//     its next states and the states their swap rules may enter, their enter actions (an ActionGroup name standing for
//     its actions), a type-43 action of kind 1 or 7). The walk runs in Tick, outside the field manager's lock
//     (a key again after 5 s, a shop's after 60 s; all forgotten once no seat is in); the pass reads only the cached
//     verdict. A key whose rows cannot be read falls back to its name containing "Shop" (logged).
//   - Press: an entry redirect on StateTrigger_Request marks the seat's press when it comes from DispatchKey's object
//     branch inside the seat's pass; world/vending then gives the shop that names the pressed NPC to that seat (its pad
//     drives it, the leader-type scope for its camera preset).
//   - The shop's AI event (ShopMenu_SendLeaderTargetAIEvent5, the NPC reacting to the leader) runs with the seat's
//     member as the leader (LeaderScope) for the seat's shop, as it does with the leader in single player.
//   - The greeting (§3 row 5): the NPC's look-at and the member's turn to face it are a PlannerEvent line whose
//     `LEADER_` the game resolves through ResolveNamedPoint_LEADER_BATTLELEADER, twice a frame from the press until
//     the shop is up (the 2026-10-25 log: 821 reads in two visits, all player 1's character while player 3 talked).
//     The resolver runs with the pressing seat's member as the leader (LeaderScope) while that seat's press is
//     pending and while its shop is up, so the shopkeeper looks at, and is faced by, the player who talks to it.
//     Logged once per window at Debug (the first read, with its caller), each read at Trace, the count at its end.
// Game thread.
//
// Per seat (players/seats.h): a press is the seat's whose pass is running (its pending
// press and the dev LEADER_ log per seat); the AI event follows the seat whose shop is up (vending's one shop owner).
// Shared: the verdicts by key (the game's data is one), read while any seat is in.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::shop_talk {

/// Finds the game's accessors and places the redirects (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with the further players out).
void Remove() noexcept;

/// Whether a seat's pass should leave the point with this key open: a shopkeeper's, while no shop is up or pending.
/// Reads only the cached verdict (no allocation, no game call: the pass holds the manager's lock); an unknown key is
/// queued for Tick. Game thread, inside the pass.
bool Admits(std::uint64_t key) noexcept;

/// Once per frame (the engine-tick callback): reads the queued keys' rows.
void Tick() noexcept;

}  // namespace lss::world::shop_talk
