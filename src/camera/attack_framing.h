#pragma once

// Which player an enemy's attack camera shows (research/RE-p2-attack-cameras.md):
//
//   - D1, "only the target's half": a battle camera sequence that frames its target and plays on every camera
//     (priority 30 enemy specials such as Granite Jail, Rude's and Elena's finishers; and the priority-10 grabs that
//     pass the game's test only through their ActorName selectors) plays only on the targeted player's camera when that
//     target is player 1's character or a seat's member (players/seats.h), and the owner is not that camera's player.
//     A target that is the AI ally or an enemy stays on every camera (the native look). The game's own activity test
//     (CamEntry_IsActive_LeaderKeyed) runs first; the redirect only ever turns its yes into a no, decided once per
//     sequence and camera (the target falls back to the AI target once a grab ends). The camera is the seat whose update
//     is open (attack_cameras::InsideSeat, the "inside seat" context: the activity test names no camera), and the
//     seat is in the latch key (LatchKey).
//   - D2, "frame the boss's target": a sequence owned by an enemy whose shot frames "the party leader" (selectors
//     PlayerLeaderMesh / PlayerLeaderActor, and the eye's relative rotation) frames the player that enemy's coin picked
//     (target_coin, the same coin that picks its actions), on every camera, instead of each camera's own player. The
//     coin is latched once per shot. A non-enemy owner, coin 0 or the coin's player gone keeps the game's own answer.
//     The end turn back to the leader is the camera operator's own subject and is untouched. The pose update and the
//     leader reads name no camera either: the shot being evaluated is kept around the update.
//
// Sites (both builds): D1 the slot-20 compute's call of the activity test; D2 the two calls of
// EndBattleCameraActor_UpdateFromOperatorPose (per camera, and the seed at play), which set the shot being evaluated,
// and the three calls of PartyManager_GetCharacterByPlayerType inside it that read the leader. Placed from the game
// thread (game_thread_sites; two cross a 16-byte block), each item whole or not at all.
// Per seat: the D1 decisions (by key) and the D2 coin counts. Game thread only.

#include "players/seats.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::camera::attack_framing {

/// D1's latch key for a sequence (its actor's weak value) on `seat`'s camera (0 player 1's): the weak value with one
/// top bit flipped per seat (seat 1 bit 63, seat 2 bit 62), so each camera decides apart. Pure.
constexpr std::uint64_t LatchKey(std::uint64_t key, int seat) noexcept
{
    return seat >= 1 && seat < players::kMaxPlayers ? key ^ (0x8000000000000000ull >> (seat - 1)) : key;
}

/// D1's rule: whether a shot of `target` stays on `seat`'s camera, with `humans` each player's character by seat (0
/// player 1's; null for a seat not in). A target that is no human's character shows on every camera (the AI ally or an
/// enemy, as solo shows it); a human's target shows on its own camera, and on another only when that camera's player
/// owns the shot (an owner equal to the camera's player, a null owner included when that player has no character, as
/// the two-player rule had it). A null target (no target yet: D1 decides it later) is no one's character. Pure.
constexpr bool ShotShowsOn(const void* target, const void* const (&humans)[players::kMaxPlayers], int seat,
                           const void* owner) noexcept
{
    bool human = false;
    for (const void* character : humans)
    {
        human = human || (target != nullptr && character == target);
    }
    if (!human)
    {
        return true;
    }
    const void* mine = seat >= 0 && seat < players::kMaxPlayers ? humans[seat] : nullptr;
    return target == mine || owner == mine;
}

/// Resolves and checks the sites and queues their placement (logged when it runs).
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out). A placement still queued then installs
/// nothing; a call crossing two blocks is restored only from the game thread, else left in place (it passes through).
void Remove() noexcept;

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::camera::attack_framing
