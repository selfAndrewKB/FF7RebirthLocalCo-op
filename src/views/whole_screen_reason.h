#pragma once

// Whether the game shows something that takes the whole screen: the decision, pure (tested on the host,
// tests/whole_screen_test.cpp), over the signals views/whole_screen.cpp reads each tick (views/whole_screen.h names
// the game state behind each one). The reason is what players/single_view.h turns the split off for.

#include <cstdint>
#include <string_view>

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
    // The minigame inputs (research/RE-p2-minigames.md):
    std::uint32_t exclusion   = 0;      // the field-exclusion mask (bit 21 minigame read; the rest logged)
    bool          minigamePlaying = false;  // on/off 36 MiniGamePlaying
    bool          fieldMinigame   = false;  // 36's setter is a field use (the chicken lure, the chocobo capture's
                                            // chase): it does not count
    bool          minigameIds     = false;  // on/off 139 crash box or 140 cactus mission (their own ids)
    bool          frogGuys        = false;  // the frog-guys manager is in a game state (2..11: load to finish)
    bool          inBattle    = false;  // the game's UEndBattleAPI::IsInBattle test
    bool          gbike       = false;  // the G-Bike manager lives
    bool          minigameMap = false;  // the location is a minigame-only map (chocobo race, coaster, G-Bike, Moogle)

    bool operator==(const ScreenSignals&) const = default;  // every field (the log's change test)
};

/// The field-exclusion bits the minigame rule reads (UEnum EndFieldExclusion).
inline constexpr std::uint32_t kExclusionMiniGame = 1u << 21;  // DisableController_MiniGame
inline constexpr std::uint32_t kExclusionSecond   = 1u << 1;   // DisableController_2nd: not read (it flashes during
                                                                // play); named for the tests of the uses that keep split

/// Whether a VR or colosseum session shows its menu or result screen: on/off 153 with the minigame bit, outside a battle
/// (research/RE-p2-minigames.md: the bit is on in the session's menu, intro and result states, off in the fight; the
/// intro runs inside the battle).
bool ArenaMenu(const ScreenSignals& signals) noexcept;

/// Whether on/off 36's setter names a field use, where the party stays with player 1: exactly the native chicken lure or
/// chocobo capture.
bool FieldUseName(std::wstring_view setter) noexcept;

/// Whether a minigame holds player 1: G-Bike or a minigame map; on/off 36 held by a minigame (not a field use); the crash
/// box's or cactus mission's own id; the frog manager in a game state; or the minigame bit outside a battle and the
/// VR / colosseum session (the entry prompts and the native flows' first states, before their 36).
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

/// EndGameState states (the transition table, research/RE-p2-retry-reload.md and RE-p2-single-view.md).
inline constexpr std::uint8_t kStateCinemaPause = 7;
inline constexpr std::uint8_t kStatePauseMenu   = 8;
inline constexpr std::uint8_t kStateChapterPause = 10;  // PAUSE_IDLE_STARTCHAPTER: the cinema pause at a chapter start

/// Whether a pause-family menu owns the game's input, by the game's own tests (research/RE-pause-menu-any-pad.md): one of
/// the three pause states, or a pause-family widget alive (IsPauseMenuClosed false: the Blueprint-opened pause menus,
/// the libra from the main menu). Every pad is the game's pad then, as in an unmodded game (input/pad_buttons.h).
constexpr bool MenuOwnsInput(std::uint8_t gameState, bool pauseWidget) noexcept
{
    return gameState == kStateCinemaPause || gameState == kStatePauseMenu || gameState == kStateChapterPause ||
           pauseWidget;
}

/// The whole-screen moments the game plays with every pad as its own, as it does a menu: a minigame, which the game runs
/// on player 1's picture through its pad-blind pollers and the merged stick (the field uses, the chicken lure and the
/// chocobo capture's chase, are not one: the party stays split there). A cutscene, a scene, a skip or the title plays
/// nothing of a pad's, and a menu has the game's own test (MenuOwnsInput above), so none of them is one.
constexpr bool SharedPlay(WholeScreenReason reason) noexcept
{
    return reason == WholeScreenReason::Minigame;
}

/// The first reason that holds, in the order above; None when nothing does or nothing is readable.
WholeScreenReason ReasonOf(const ScreenSignals& signals) noexcept;

/// For the log.
const wchar_t* NameOf(WholeScreenReason reason) noexcept;

}  // namespace lss::views
