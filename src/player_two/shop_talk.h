#pragma once

// Player 2 uses a shopkeeper's shop with the game's own field prompt and shop (RE-p2-shopkeeper-talk.md; the user,
// 2026-10-18: "extend shop keeper interaction to P2 as well, similar to the vending machine").
//
// A shopkeeper is a CharaPop ObjectActor NPC with an ordinary field-action point (key = the CharaPop key, e.g.
// oba2100_ShopWeapon01). Pressed, the game's DispatchKey fires the NPC's state trigger (StateTrigger_Request, no leader
// read); the NPC's next state then runs, on later frames and outside player 2's pass, an optional greeting and a
// "show menu" action (type 43; kind 1 FEndShowShopMenu, kind 7 FEndShowSkillTreeMenu), the same shop command vending
// machines use.
//   - Admit: player 2's field pass (player_two/field_pickup) leaves a point open when its key is a shopkeeper's, read
//     from the game's own data with the game's accessors (§1.2: the GS object's talk state, its next state, that
//     state's enter actions, a type-43 action of kind 1 or 7). The walk runs in Tick, outside the field manager's lock
//     (a key again after 5 s, a shop's after 60 s; all forgotten when player 2 leaves); the pass reads only the cached
//     verdict. A key whose rows cannot be read falls back to its name containing "Shop" (logged).
//   - Press: an entry redirect on StateTrigger_Request marks player 2's press when it comes from DispatchKey's object
//     branch inside player 2's pass; player_two/vending then gives the shop that names the pressed NPC to player 2 (pad 2
//     drives it, the leader-type scope for its camera preset).
//   - The shop's AI event (ShopMenu_SendLeaderTargetAIEvent5, the NPC reacting to the leader) runs with player 2's
//     member as the leader (LeaderScope) for player 2's shop, as it does with the leader in single player.
//   - Logging only: during a pending press, the game's LEADER_ resolver (ResolveNamedPoint_LEADER_BATTLELEADER) is
//     counted with its callers, to settle who the greeting looks at (§3 row 5) before any change there.
// Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::shop_talk {

/// Finds the game's accessors and places the redirects (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Whether player 2's pass should leave the point with this key open: a shopkeeper's, while no shop is up or pending.
/// Reads only the cached verdict (no allocation, no game call: the pass holds the manager's lock); an unknown key is
/// queued for Tick. Game thread, inside the pass.
bool Admits(std::uint64_t key) noexcept;

/// Once per frame (the engine-tick callback): reads the queued keys' rows.
void Tick() noexcept;

}  // namespace lss::player_two::shop_talk
