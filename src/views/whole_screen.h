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
//   - a minigame that takes player 1 away to its own stage (the user's rule, 2026-10-15: the chicken lure,
//     where the party stays in the field with player 1, stays split; frog jumping goes single). The game's own
//     switch for it is its field-exclusion mask: bit 21 DisableController_MiniGame, bit 1 DisableController_2nd
//     (RE-p2-minigames.md §1, §7), guarded by the game's own battle test and the colosseum flag, plus the
//     minigames that set no bit (G-Bike's manager, the minigame-only maps).
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
    bool          pauseWidget = false;  // a pause-family widget is alive (IsPauseMenuClosed is false)
    std::uint16_t nativeMenus = 0;      // NativeMenu bits: script menus with their own EndUISystem slot open
    bool          screenWidget = false; // a full-screen Blueprint menu is on screen (views/screen_widgets.h)
    bool          colosseumFlag = false;  // on/off 153 ShowColosseumMenu: a VR or colosseum session (Begin to End)
    // For the log only (candidates the open log is to settle, RE-p2-menu-census.md E6, RE-p2-vr-menu.md):
    bool          miniGameMenu  = false;  // on/off 92 SoundCallEventMiniGameMenuInOut
    std::int8_t   chadleyState  = -1;     // Chadley's manager state (0 idle, 1 greeting, 2 shop, 3 VR, 4 farewell, 5 end)
    // The minigame inputs (RE-p2-minigames.md §7.6):
    std::uint32_t exclusion   = 0;      // the field-exclusion mask (bit 21 minigame, bit 1 the second-purpose bit)
    bool          minigameIds = false;  // on/off 36 MiniGamePlaying, 139 crash box or 140 cactus mission
    bool          fieldMinigame = false;  // the only one on is 36, held by a field minigame (the chicken lure, the chocobo
                                          // capture's chase): it does not count (S-FRAMINGVERIFY 5e/5f)
    bool          frogGuys    = false;  // the frog-guys manager holds its frog, or it set id 23
    bool          inBattle    = false;  // the game's UEndBattleAPI::IsInBattle test
    bool          gbike       = false;  // the G-Bike manager lives
    bool          minigameMap = false;  // the location is a minigame-only map (chocobo race, coaster, G-Bike, Moogle)
};

/// The field-exclusion bits the minigame rule reads (UEnum EndFieldExclusion).
inline constexpr std::uint32_t kExclusionMiniGame = 1u << 21;  // DisableController_MiniGame
inline constexpr std::uint32_t kExclusionSecond   = 1u << 1;   // DisableController_2nd

/// Whether a VR or colosseum session shows its menu or result screen: on/off 153 with the minigame bit, outside a battle
/// (RE-p2-minigames.md §7.2: the bit is on in the session's menu, intro and result states, off in the fight; the
/// intro runs inside the battle).
bool ArenaMenu(const ScreenSignals& signals) noexcept;

/// Whether a minigame has taken player 1 to its own stage (§7.6): G-Bike or a minigame map; or the minigame bit, outside
/// the colosseum / VR battles (on/off 153) and outside a battle unless a minigame id is on (the cactus mission is a
/// battle); or the second-purpose bit with a minigame id or frog guys (the buggy, summon bosses, the gravity device and
/// program actions also use that bit, without them).
bool MinigameStage(const ScreenSignals& signals) noexcept;

/// ScreenSignals::nativeMenus bits.
enum NativeMenu : std::uint16_t
{
    kJukebox        = 1 << 0,
    kCarriage       = 1 << 1,
    kPointWarp      = 1 << 2,
    kCardDeckNpc    = 1 << 3,
    kChocoboStable  = 1 << 4,
    kChocoboSelect  = 1 << 5,
    kChocoboRace    = 1 << 6,
    kCardChallenge  = 1 << 7,
    kBrawlerSelect  = 1 << 8,
    kBrawlerWinner  = 1 << 9,
};

enum class WholeScreenReason : std::uint8_t
{
    None,
    PauseMenu,    // EndGameState 8
    CinemaPause,  // EndGameState 7
    ScriptMenu,
    JobBoard,
    PauseWidget,
    NativeMenu,
    ScreenWidget,
    ArenaMenu,
    Minigame,
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
