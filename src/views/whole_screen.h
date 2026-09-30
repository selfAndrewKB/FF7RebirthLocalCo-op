#pragma once

// Whether the game shows something that takes the whole screen, from the game's own state
// (RE-p2-single-view.md §2): the reason players/single_view.h turns the split off.
//
//   - the EndGameState machine (Square Enix): state 8 is "a pause menu owns the screen" (the camp /
//     main menu, the map journal, battle pause, game over, colosseum, the script-opened pause and map
//     menus), state 7 the pause during a cutscene; the menu's kind names which one;
//   - the on/off table (Square Enix's EndFieldOnOffTable reference counts): the script-opened menus,
//     shops and the skill tree, which do not all go through the state machine; and id 12
//     ShowSkipPauseMenu, which the game's dialogue and camera-planner scenes hold on while they can be
//     skipped (ordinary NPC talk cannot, so it stays split);
//   - the job board, opened by its own script command, which keeps it in a weak pointer and runs while
//     that pointer is valid (§7.1): the same test here;
//   - the cutscene manager: a sequence of type 0 or 2 playing (the game's own check, used by 92 of
//     its callers), or a cutscene skip under way;
//   - the title: the game state's current location is "0005-TITLB", the comparison the game itself
//     makes (§7.3).
//
// The decision is pure, so it is tested on the host; the reads are in whole_screen.cpp.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views {

/// The game's signals, as read this tick.
struct ScreenSignals
{
    bool          readable   = false;  // the game state was found (false: nothing below is known)
    std::uint8_t  gameState  = 0;      // EndGameState state
    std::uint8_t  menuKind   = 0;      // EEndSystemPauseMenuKind
    bool          scriptMenu = false;  // an on/off menu id is on (main menu, shop, map, skill tree...)
    bool          jobBoard   = false;  // the job board's weak pointer is valid
    bool          cutscene   = false;  // the cutscene manager plays a sequence of type 0 or 2
    bool          skippable  = false;  // on/off id 12: a skippable dialogue or camera-planner scene
    bool          skipping   = false;  // a cutscene skip is under way (the game holds black)
    bool          title      = false;  // the current location is the title
};

enum class WholeScreenReason : std::uint8_t
{
    None,
    PauseMenu,    // EndGameState 8
    CinemaPause,  // EndGameState 7
    ScriptMenu,
    JobBoard,
    Cutscene,
    SkippableScene,
    CutsceneSkip,
    Title,
};

/// EndGameState states (the transition table, RE-p2-retry-reload.md §2 and RE-p2-single-view.md §2.1).
inline constexpr std::uint8_t kStateCinemaPause = 7;
inline constexpr std::uint8_t kStatePauseMenu   = 8;

/// The first reason that holds, in the order above; None when nothing does or nothing is readable.
WholeScreenReason ReasonOf(const ScreenSignals& signals) noexcept;

/// For the log.
const wchar_t* NameOf(WholeScreenReason reason) noexcept;

/// Checks the game's code and data the reads rely on; false (the automatic single view stays off,
/// logged) if any differs.
bool InstallWholeScreen(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick: this tick's signals (zeroed if not installed).
ScreenSignals ReadScreenSignals() noexcept;

/// Game thread: whether the game's EndGameState exists now (its own getter, checked at install).
/// False if not installed or the getter failed.
bool GameStatePresent() noexcept;

}  // namespace lss::views
