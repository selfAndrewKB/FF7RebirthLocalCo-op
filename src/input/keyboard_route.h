#pragma once

// The engine's half of the keyboard and mouse following the player the keyboard plays (research/RE-kbm-engine-routing.md).
//
// The scene viewport hands every key, mouse button, wheel turn and mouse move to the game viewport client's InputKey
// and InputAxis with the Slate user as the controller id (0 for the keyboard and mouse, the mouse axes always 0); each
// finds the local player with that controller id and gives the input to that player's controller, whose own player
// input then answers the mouse look (GetInputMouseDelta). The mod takes those two entries: while the keyboard plays
// player 2 or 3 alone (input/pad_table), an input that is not a pad's goes on with that player's controller id, so
// their camera turns with the mouse and player 1's reads none. Everything after the entries is the engine's own.
// Square's own tables carry the keyboard by its source bits instead (input/button_view, input/pad_sticks).

namespace lss::platform {
class GameImage;
}

namespace lss::input::keyboard_route {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

#if LSS_DEV
/// The `players.keyboard-route` reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::input::keyboard_route
