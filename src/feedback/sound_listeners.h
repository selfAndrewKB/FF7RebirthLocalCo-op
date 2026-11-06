#pragma once

// Every player hears the sounds near them: each SEAD sound is measured from the nearest player's listener
// (research/RE-p2-audio-listener.md for the mechanism, RE-listener-per-seat.md for every player; plan/seats.md).
//
// UE's stock viewport draw sets one audio listener per view, in the local player list's order (FAudioDevice listeners
// [0], [1], [2]: one per column, views/columns.h). Square Enix's SEAD audio device pushes only listener [0] into SEAD's
// one listener (SQEX::Sd::Lay::Listener), which every SEAD sound measures its audibility, gain, pan and doppler from, so
// a sound near a seat's player and far from player 1 was never voiced. Rebirth has no getter for that listener (25
// inline loads of its global), so each lever runs the game's own code with SEAD's listener pointer swapped to another
// player's listener and restored, on SEAD's update thread:
//   - a listener per seat: a copy of the game's listener object, set every SEAD frame by the game's own
//     setter from UE's listener at that seat's column (read where the device pushes [0]) and given its velocity by
//     the game's own update. It is on while the player is in, their own view is on screen (views/column_source.h
//     Self: never a column on player 1's picture, never under the engine's single view), and the engine's listener at
//     that column follows their own camera (an entry the engine stopped updating turns it off);
//   - moving, placed and wedge sounds: the audibility test takes the nearest listener (a sound keeps its listener while
//     within its range until another is 20 % nearer, so its pan does not flip between two players), and the sound's
//     update (doppler, gain, pan, the voice's start and stop) runs from the listener its test chose. One voice per
//     sound, in the shared mix, panned from that player's camera;
//   - the AutoSe listener position (SEAD's one reader of that pointer off SEAD's thread) never sees a swap: it retries
//     around one, or gives the latest clean answer. Its two callers take the nearest player's listener: the footstep
//     detection's distance clip (gate A: the character the processor update's frame holds) and the rustle and swing
//     range measure (gate C: the point the measure reads), through AutoSe's own converter;
//   - the footstep detection's LOD (gate B, only for characters with a LOD setting): the game's level is computed again
//     from each seat's PC listener, and the lowest level is kept. Only when asked on the game thread (it
//     reads the seats' controllers): AutoSe ran on other threads live (2026-10-27), so a level asked there is counted
//     ("off the game thread") and kept as the game's; no LOD has been asked in play so far.
// Unchanged: area sounds (polygon, sector: their test measures a shape's point nearest the listener; counted only),
// ambient and direction sounds (no place to be near), listener-relative moving sounds, the obstruction pass (player
// 1's place) and the singleton itself. Nothing changes while there is one UE listener.
// Nothing is formatted, locked or logged off the game thread: the audio and SEAD threads write atomics and seqlocks.
// Logged: the install, each seat's listener on or off with the reason, SEAD setting it or not, the engine's
// listener count (Info); each hook's thread once (Debug); a summary every 10 s (Debug): per sound class the sounds
// measured from each listener and heard, and those cut while within another player's reach (the check that the fix
// works: about 0); the updates per listener; the clip and the measure per listener; the LOD. The listeners each tick
// (Trace).

#include "players/seats.h"

#include <array>

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::sound_listeners {

/// Replaces the slots and redirects (each checked; logged). The player controller's listener slot is replaced later,
/// from Tick, once player 1's controller exists.
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, every engine tick: the controller slot, each seat's listener, the state changes and the
/// summary. `ownColumns[seat]`: the column showing that seat's own view (its index in the engine's local player list,
/// views/columns.h, while views/column_source.h gives it Self), -1 otherwise; index 0 (player 1's) unused.
void Tick(const std::array<int, players::kMaxPlayers>& ownColumns) noexcept;

}  // namespace lss::feedback::sound_listeners
