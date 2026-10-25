#pragma once

// Player 2's half has its own sound: each SEAD sound is measured from the nearer of the two players' listeners
// (research/RE-p2-audio-listener.md §7).
//
// UE's stock viewport draw sets one audio listener per view, in the local player list's order (FAudioDevice listeners
// [0], [1], ...: one per column, views/columns.h). Square Enix's
// SEAD audio device pushes only listener [0] into SEAD's one listener (SQEX::Sd::Lay::Listener), which every SEAD sound
// measures its audibility, gain, pan and doppler from, so a sound near player 2 and far from player 1 was never voiced.
// Three levers, each running the game's own code for player 2:
//   - L1, a second listener: a copy of the game's listener object, set every SEAD frame by the game's own setter from
//     UE's listener at player 2's column (player 2's camera, read where the device pushes [0]; never a mirror view's,
//     which sits on player 1's camera) and given its velocity by the game's own
//     update. For each moving or placed sound the nearer listener is chosen (with hysteresis), and the game's
//     audibility test and update (doppler, gain, pan, the voice's start and stop) run with SEAD's listener pointer
//     swapped to it, then restored, all on SEAD's update thread. One voice per sound, as before.
//   - The AutoSe listener position (SEAD's one reader of that pointer off SEAD's thread: the footstep detection's
//     distance clip and its warp check) never sees a swap: it retries around one, or gives the latest clean answer.
//   - L2, the footstep detection's distance clip (gate A): inside the AutoSe processor update, whose frame holds the
//     character, the clip gets player 2's listener (through AutoSe's own converter) when it is nearer the character,
//     so player 2's chocobo footsteps (their sound and rumble) are detected near player 2 as near player 1.
//   - L3, the footstep detection's LOD (gate B, only for characters with a LOD setting): the game's level is computed
//     a second time from player 2's PC listener, and the lower level is kept.
// Nothing changes while there is one UE listener. Logged: install (Info); player 2's listener on and off, the engine's
// listener count on change, a 10 s summary (Debug). Nothing is formatted, locked or written to the log off the game
// thread (the skill's audio-thread rule).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::audio_listener_two {

/// Replaces the slots and redirects (each checked; logged). The player controller's listener slot is replaced later,
/// from Tick, once player 1's controller exists.
void Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, every engine tick: the controller slot, the state changes and the summary (Debug). `columnTwo`: the
/// column of the further player the second listener follows (its index in the engine's local player list,
/// views/columns.h; the first further player in, modules.cpp), -1 for none: the listener the audio thread copies.
/// One second listener: with three players in, player 3's sounds are measured from player 1's or player 2's.
void Tick(int columnTwo) noexcept;

}  // namespace lss::player_two::audio_listener_two
