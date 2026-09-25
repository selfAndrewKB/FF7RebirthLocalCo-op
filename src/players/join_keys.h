#pragma once

// The keys that express player intent:
//   Ctrl+F7  add player 2            Ctrl+F8  remove player 2
//   Ctrl+F5  top/bottom <-> side by side
//   Ctrl+F4  halves that fill <-> halves kept 16:9 with bars
//   Ctrl+F6  player 2's half: player 2's own camera (at join) <-> player 1's camera (testing)
//   Ctrl+F9  each pad drives only its own player's sticks (on at start; off merges them, for testing)
// Keys never reach renderer internals; the renderer follows the player count.

namespace lss::players {

enum class PlayerIntent
{
    None,
    Join,
    Leave,
    ToggleLayout,
    ToggleFill,
    ToggleOwnCamera,
    TogglePadSticks,
};

class JoinKeys
{
public:
    /// Reads the keys once per tick. Reports an intent on the press, not while held, and only
    /// while the game window has focus.
    PlayerIntent Poll() noexcept;

private:
    bool joinWasDown_   = false;
    bool leaveWasDown_  = false;
    bool cameraWasDown_ = false;
    bool layoutWasDown_ = false;
    bool fillWasDown_   = false;
    bool sticksWasDown_ = false;
};

}  // namespace lss::players
