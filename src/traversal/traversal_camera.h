#pragma once

// Player 2's traversal cameras (research/RE-p2-traversal-L3-ladder-camera.md, RE-p2-traversal-cameras.md).
//
// Every traversal camera goes on the PRIMARY camera's slot 15 and only for "the player" (IsUCPC_Core, or the move
// gate FieldActionMove_IsPlayerChar). For player 2's member the game answers no outside the mod's scopes, so player 2
// climbs with the follow camera. Two shapes:
//
//  - Executer classes (ladder, squeeze, crawl, pole slide, lifting rope, one-shot move): the executer's Start / Tick /
//    End install and switch off the camera on the game thread. For an executer whose character (+0x10) is player 2's
//    member they run inside input::member_movement::InPrimaryScope (player 2's controller as primary, the member's leader
//    answer), plus a LeaderScope where the installer reads the leader (squeezes always, rail-camera actors at the
//    install). The actor's camera handle is shared by both players, so player 2's own copy is swapped in around each
//    call: player 1's use of the same actor keeps its camera and never switches player 2's off. Where the game's exit
//    switches the camera off from the anim worker behind IsUCPC (ladder blend, squeeze, crawl), the same off runs once
//    for player 2 at the Tick that sees the exit, in player 2's scopes. The one-shot move has no handle: its actor's
//    leave runs the game's own off inside climb_context's camera context (player 2's camera manager on that thread).
//
//  - Move classes (climbing wall, monkey bars, spider wall): the move's Begin installs through the actor's camera
//    holder (+0x6D0, which records the installing character), on the game thread. Player 2's Begin runs in the same
//    scopes and installs on player 2's own holder for that actor (the game's holder functions run on it), so neither
//    player's climb evicts the other's camera; the actor's leave and EndPlay release it. The per-frame camera rotation
//    of monkey bars and spider walls is gated by the move gate on the anim worker; its call answers yes for player 2's
//    installed move. The climbing wall's hip framing follows player 2's hold phases.
//
// The ladder executer's Start, End and UpdateInput slots belong to traversal_watch, which calls the functions below;
// everything else is replaced here. Game thread, except the rotation gate, the holder getter and the one-shot leave
// (any thread: plain compares and the game's own calls).
//
// Units: traversal_facts.h (the classes' tables), traversal_exec.cpp (the executer classes), traversal_holders.cpp
// (the move classes and the seats' holders), traversal_collision.cpp (the collision watch, dev tools) and
// traversal_shared.h (what they share); this file's Install, Remove and Tick compose them (traversal_camera.cpp).
//
// Per seat (players/seats.h): what is described above for player 2 is kept for each seat (the camera handle copies by
// seat and actor, a holder pool per seat, the ladder exit, the installed move); the seat is the one whose member the
// executer's or move's character is, or climb_context's climber on the anim worker. Still player 2's: the one-shot
// actor's leave (climb_context's WithPlayerTwoCamera), and the ladder's UpdateInput window, whose scopes
// traversal_watch opens for player 2. The collision watch is a dev tool and stays two-player.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::traversal::traversal_camera {

/// Replaces the slots and the call sites (each checked; a class whose check fails keeps the follow camera, logged).
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, per frame: for each built seat, an idle holder of the seat's is released (all of them with the seat
/// out); with the dev tools, the collision watch of player 1's and player 2's traversal cameras (Debug, a line per
/// camera).
void Tick() noexcept;

using ExecFn = std::uintptr_t (*)(void* executer);

/// The ladder executer's Start and End, called by traversal_watch's slot replacements with the game's own function.
std::uintptr_t LadderStart(ExecFn original, void* executer) noexcept;
std::uintptr_t LadderEnd(ExecFn original, void* executer) noexcept;

using LadderInputFn = std::uintptr_t (*)(void* executer, float dt);

/// The ladder executer's UpdateInput (window B, the seat's scopes held by the caller): a seat's executer runs with
/// the seat's ladder camera handle in the ladder, so the slide flag lands on the seat's camera.
std::uintptr_t LadderInput(LadderInputFn original, void* executer, float dt) noexcept;

}  // namespace lss::traversal::traversal_camera
