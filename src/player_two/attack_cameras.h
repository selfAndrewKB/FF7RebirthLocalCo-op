#pragma once

// The game's attack cameras (battle camera sequences: grabs, enemy specials, bursts) on each player's own camera
// (research/RE-p2-attack-cameras.md §1, §9).
//
// The game keeps one list of playing sequences (the battle system's +0x50), and every camera manager's Skill module
// (slot 20 of its Battle and Field operators) tests each entry against "the party leader" during its own update. Player
// 2's update runs in player 2's leader view (field_camera RunAsPrimary), so the game already decides per player which
// camera a grab belongs to. What leaked is the list's own per-camera state, kept once because solo has one camera:
//   - the smoothing (+0x70 previous focus point, +0x80 previous camera location, +0x48 the last active priority): a
//     camera with no active entry writes its own pose there each frame, and a camera with one lerps from there toward
//     the shot. With both cameras on the one copy, player 2's grab shot started from player 1's camera every frame and
//     held player 2's view near player 1's (the Titan report of 2026-10-15);
//   - each entry's active byte (+0x14), which code outside the updates reads (a stop by id, the leader change, the Menu
//     operator): after both updates it held player 2's view.
// A PlayerTwoUpdate around player 2's update puts player 2's own copy of the smoothing and of the active bytes in the
// list, and after it player 1's smoothing back and each entry's byte as "active on either camera" (what the readers
// outside the updates need: a sequence one camera still plays is playing). Each camera reads only its own state (solo
// in each half; the game's decision is untouched).
//
// The shot change: a sequence cut asks UWorld::GetFirstPlayerController's camera manager for a camera cut, so a cut in
// player 2's update cut player 1's view (its TAA / motion-blur history) and left player 2's smeared. Inside player 2's
// update that call goes to the game's own "cut the primary camera" function instead, which the primary-controller scope
// points at player 2's camera manager (§9.3).
//
// Game thread only. Nothing is written outside player 2's update.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::attack_cameras {

/// Finds the list and the cut call. Logged; a failed part stays off.
void Install(const platform::GameImage& image) noexcept;

/// Player 2's own per-camera state of the sequence list while it lives (inside player 2's camera update, its scopes
/// held); player 1's put back when it ends. Does nothing if not installed, off the game thread, or there is no list.
class PlayerTwoUpdate
{
public:
    PlayerTwoUpdate() noexcept;
    ~PlayerTwoUpdate();
    PlayerTwoUpdate(const PlayerTwoUpdate&)            = delete;
    PlayerTwoUpdate& operator=(const PlayerTwoUpdate&) = delete;

private:
    void* list_ = nullptr;
};

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::player_two::attack_cameras
