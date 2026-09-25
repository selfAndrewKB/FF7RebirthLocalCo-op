#pragma once

// Each pad's buttons for its own player (RE-p2-per-pad-buttons.md). Step 1: logging only.
//
// Square's input object (the one that holds the sticks, player_two/pad_sticks.h) keeps every
// gameplay action in a table at +0xF8, with a UI-filtered twin at +0x148. Each 40-byte element is
// an FName key and, at +0x18, the merged state: previous-held, held, repeat, a changed-this-frame
// latch, and a 32-bit mask of the sources holding it (XInput pad N sets bit min(N, 3), DirectInput
// pad N bit 21 + N). The game reads "pressed" as previous == 0 and held == 1, so the edges come
// from the merged state. The key handlers write the tables while the devices are polled, and the
// input processor's Tick advances previous = held once per frame, all before the world ticks.
//
// This step replaces the Tick's slot (IInputProcessor vtable, slot 1) with one that calls the
// game's Tick and then logs, while player 2 is in, every element whose held state or source mask
// changed since the last frame, by name. It writes nothing. It is there to confirm live that pad 2
// sets its own bit before the per-pad view is built on it.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::pad_buttons {

/// Replaces the Tick slot after checking it. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The status line: changes logged and dropped since the last call, and the tables' sizes.
void LogStatus() noexcept;

}  // namespace lss::player_two::pad_buttons
