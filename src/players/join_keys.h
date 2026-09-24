#pragma once

// The two keys that express player intent (philosophy §4.2): Ctrl+F7 adds player 2, Ctrl+F8
// removes player 2. Keys never reach renderer internals; the renderer follows the player count.

namespace lss::players {

enum class PlayerIntent
{
    None,
    Join,
    Leave,
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
};

}  // namespace lss::players
