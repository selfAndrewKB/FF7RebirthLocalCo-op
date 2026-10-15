#pragma once

// Which player an enemy's attack camera shows, decided by the user (2026-10-15; research/RE-p2-attack-cameras.md §10,
// S-ATKCAMLEADER):
//
//   - D1, "only the target's half": a battle camera sequence that frames its target and plays on every camera
//     (priority 30 enemy specials such as Granite Jail, Rude's and Elena's finishers; and the priority-10 grabs that
//     pass the game's test only through their ActorName selectors) plays only on the targeted player's camera when that
//     target is player 1's character or player 2's member, and the owner is not the other camera's player. A target
//     that is the AI ally or an enemy stays on both cameras (the native look). The game's own activity test
//     (CamEntry_IsActive_LeaderKeyed) runs first; the redirect only ever turns its yes into a no, decided once per
//     sequence and camera (the target falls back to the AI target once a grab ends).
//   - D2, "frame the boss's target": a sequence owned by an enemy whose shot frames "the party leader" (selectors
//     PlayerLeaderMesh / PlayerLeaderActor, and the eye's relative rotation) frames the player that enemy's coin picked
//     (target_coin, the same coin that picks its actions), on both cameras, instead of each camera's own player. The
//     coin is latched once per shot. A non-enemy owner, coin 0 or player 2 gone keeps the game's own answer. The end
//     turn back to the leader is the camera operator's own subject and is untouched.
//
// Sites (both builds): D1 the slot-20 compute's call of the activity test; D2 the two calls of
// EndBattleCameraActor_UpdateFromOperatorPose (per camera, and the seed at play), which set the shot being evaluated,
// and the three calls of PartyManager_GetCharacterByPlayerType inside it that read the leader. Placed from the game
// thread (game_thread_sites; two cross a 16-byte block), each item whole or not at all. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::attack_framing {

/// Resolves and checks the sites and queues their placement (logged when it runs).
void Install(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::player_two::attack_framing
