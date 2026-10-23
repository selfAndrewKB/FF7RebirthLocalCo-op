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
//     makes (§7.3);
//   - the pause-menu family's widgets, by the game's own UEndMenuAPI::IsPauseMenuClosed: also the pause
//     modes a Blueprint opens without the state machine, and script-shown widgets (RE-p2-menu-census.md E1);
//   - the script menus that keep their widget in their own EndUISystem slot (jukebox, chocobo carriage,
//     point warp, chocobo stable and race menus, the Queen's Blood NPC deck and challenge, the 3D Brawler
//     screens), each by the game's own "still open" test for it (E2);
//   - the full-screen Blueprint menus no native code reads (photo mode): the widgets as the game adds them
//     to the screen (views/screen_widgets.h);
//   - the VR and colosseum menus and result screens (Chadley's combat simulator, the Gold Saucer colosseum):
//     the session's on/off 153 with the minigame bit, outside a battle (the game sets the bit in the session's
//     menu, intro and result states and clears it for the fight; the intro is already a battle);
//   - a minigame (the user's rule, 2026-10-15/16: single, except the field uses where the party stays with player 1:
//     the chicken lure and the chocobo capture's chase; the Temple's gravity rooms and the desert buggy course go
//     single). The game's own switch is
//     on/off 36 MiniGamePlaying: the event runner holds it from before an action's opening fade to after its closing
//     fade, named after the action, and the native flows hold it for their game (RE-p2-minigames.md §9-§10,
//     RE-p2-minigames-stage2.md). The field-exclusion bits only flash during play (frog jumping and Run Wild clear
//     them for the play state), so they cover only what comes before a game's 36: bit 21 DisableController_MiniGame
//     outside battles and the VR / colosseum session. Plus the crash box's and cactus mission's own ids (139, 140),
//     the frog manager's own state, G-Bike's manager and the minigame-only maps.
//
// The decision is in views/whole_screen_reason.h, tested on the host; the reads are in whole_screen.cpp.

#include "views/whole_screen_reason.h"

namespace lss::platform {
class GameImage;
}

namespace lss::views {

/// Checks the game's code and data the reads rely on; false (the automatic single view stays off,
/// logged) if any differs.
bool InstallWholeScreen(const platform::GameImage& image) noexcept;

/// Puts back what screen_widgets redirected (at mod uninstall); the reads patch nothing.
void RemoveWholeScreen() noexcept;

/// Whether the checks passed: the automatic single view and the streaming views' update read it.
bool WholeScreenInstalled() noexcept;

/// Game thread: the game shows something that takes the whole screen now (a full-screen menu, the map screen, a
/// cutscene), from this tick's signals; false if not installed. Each change of reason is logged by the read.
bool WholeScreenShown() noexcept;

/// Game thread, once per engine tick: this tick's signals (zeroed if not installed).
ScreenSignals ReadScreenSignals() noexcept;

/// Game thread: whether the game's EndGameState exists now (its own getter, checked at install).
/// False if not installed or the getter failed.
bool GameStatePresent() noexcept;

}  // namespace lss::views
