#pragma once

// The keys that express player intent (philosophy §4.2): Ctrl+F7 adds player 2, Ctrl+F8 removes
// player 2. Keys never reach renderer internals; the renderer follows the player count.
//
// Ctrl+F6 is a labelled TEST INSTRUMENT, not player intent: it turns player 2's view 30° away
// from player 1's for the crash test (SPLITSCREEN-ARCHITECTURE.md §9), and is removed when player 2
// has a camera of its own.

namespace lss::players {

enum class PlayerIntent
{
    None,
    Join,
    Leave,
    ToggleTestTurn,  // test instrument (see above)
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
};

}  // namespace lss::players
