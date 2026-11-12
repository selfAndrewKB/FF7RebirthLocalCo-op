#pragma once

// The game's attack cameras (battle camera sequences: grabs, enemy specials, bursts) on each player's own camera
// (research/RE-p2-attack-cameras.md).
//
// The game keeps one list of playing sequences (the battle system's +0x50), and every camera manager's Skill module
// (slot 20 of its Battle and Field operators) tests each entry against "the party leader" during its own update. A
// seat's update runs in its own leader view (field_camera RunAsPrimary), so the game already decides per player which
// camera a grab belongs to. What leaked is the list's own per-camera state, kept once because solo has one camera:
//   - the smoothing (+0x70 previous focus point, +0x80 previous camera location, +0x48 the last active priority): a
//     camera with no active entry writes its own pose there each frame, and a camera with one lerps from there toward
//     the shot. With every camera on the one copy, a seat's grab shot started from player 1's camera every frame and
//     held that seat's view near player 1's (seen in a Titan fight);
//   - each entry's active byte (+0x14), which code outside the updates reads (a stop by id, the leader change, the Menu
//     operator): after the updates it held the last seat's view.
// A SeatUpdate around a seat's update (players/seats.h) puts that seat's own copy of the smoothing and of the
// active bytes in the list, and after it player 1's smoothing back and each entry's byte as "active on any camera" (what
// the readers outside the updates need: a sequence one camera still plays is playing). Each camera reads only its own
// state (solo in each view; the game's decision is untouched). Seats take turns: one seat's update closes before the
// next opens, each OR-ing its own bytes into what it found [ASSUMPTION: the engine updates player 1's camera manager
// before the seats', as the two-player mod relies on].
//
// The shot change: a sequence cut asks UWorld::GetFirstPlayerController's camera manager for a camera cut, so a cut in
// a seat's update cut player 1's view (its TAA / motion-blur history) and left that seat's smeared. Inside a seat's
// update that call goes to the game's own "cut the primary camera" function instead, which the primary-controller scope
// points at that seat's camera manager. The cut call names no camera: it reads the seat whose update is open
// (InsideSeat, the "inside seat" context).
//
// Per seat: the copy, its list and the entries its camera found active; the counts. Shared: the open update's saved
// player-1 state (one update at a time). Game thread only. Nothing is written outside a seat's update.

#include "players/seats.h"

namespace lss::platform {
class GameImage;
}

namespace lss::camera::attack_cameras {

/// Finds the list and the cut call. Logged; a failed part stays off.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out).
void Remove() noexcept;

/// `seat`'s own per-camera state of the sequence list while it lives (inside the seat's camera update, its scopes
/// held); player 1's put back when it ends. Does nothing if not installed, off the game thread, inside another seat's
/// update, or there is no list.
class SeatUpdate
{
public:
    explicit SeatUpdate(int seat) noexcept;
    ~SeatUpdate();
    SeatUpdate(const SeatUpdate&)            = delete;
    SeatUpdate& operator=(const SeatUpdate&) = delete;

private:
    void* list_ = nullptr;
    int   seat_ = 0;
};

/// Game thread: the seat whose camera update is open (a SeatUpdate holds the list), so "this camera" is that
/// seat's; 0 outside every seat's update (player 1's camera).
int InsideSeat() noexcept;

/// Game thread, once per engine tick: the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::camera::attack_cameras
