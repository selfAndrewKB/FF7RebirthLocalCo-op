#pragma once

// The keys that express player intent: Ctrl+F7 adds player 2, Ctrl+F8 removes player 2, Ctrl+F5
// switches the split between top/bottom and side by side, Ctrl+F4 switches between halves that
// fill and halves kept 16:9 with bars. Keys never reach renderer internals; the renderer follows
// the player count.
//
// Ctrl+F6 is a TEST key, not player intent: it turns player 2's view 30° away from player 1's, so
// two different views can be tested before player 2 has a camera of its own.

namespace lss::players {

enum class PlayerIntent
{
    None,
    Join,
    Leave,
    ToggleLayout,
    ToggleFill,
    ToggleTestTurn,  // test key (see above)
};

class JoinKeys
{
public:
    /// Reads the keys once per tick. Reports an intent on the press, not while held, and only
    /// while the game window has focus.
    PlayerIntent Poll() noexcept;

private:
    bool joinWasDown_  = false;
    bool leaveWasDown_ = false;
    bool turnWasDown_  = false;
    bool layoutWasDown_ = false;
    bool fillWasDown_   = false;
};

}  // namespace lss::players
