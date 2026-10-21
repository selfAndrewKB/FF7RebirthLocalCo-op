// Host-side tests for views::ReasonOf, run without the game.
//
// The cases: nothing known means no reason; each signal alone gives its reason; the pause-menu state
// wins over the others; the other game states (the field running, a load) give none.

#include "views/whole_screen.h"

#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

using lss::views::ReasonOf;
using lss::views::ScreenSignals;
using lss::views::WholeScreenReason;

ScreenSignals Readable()
{
    ScreenSignals signals;
    signals.readable  = true;
    signals.gameState = 6;  // the field running
    return signals;
}

void TestNothingKnown()
{
    std::puts("whole screen: an unreadable game state gives no reason, whatever else is set");
    ScreenSignals signals;
    signals.gameState = lss::views::kStatePauseMenu;
    signals.cutscene  = true;
    signals.title     = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::None);
    EXPECT(ReasonOf(Readable()) == WholeScreenReason::None);
}

void TestEachSignal()
{
    std::puts("whole screen: each signal alone gives its reason");
    auto signals = Readable();
    signals.gameState = lss::views::kStatePauseMenu;
    EXPECT(ReasonOf(signals) == WholeScreenReason::PauseMenu);
    signals.gameState = lss::views::kStateCinemaPause;
    EXPECT(ReasonOf(signals) == WholeScreenReason::CinemaPause);

    signals = Readable();
    signals.scriptMenu = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScriptMenu);

    signals = Readable();
    signals.cutscene = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::Cutscene);

    signals = Readable();
    signals.skipping = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::CutsceneSkip);

    signals = Readable();
    signals.jobBoard = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::JobBoard);

    signals = Readable();
    signals.skippable = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::SkippableScene);

    signals = Readable();
    signals.title = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::Title);
}

void TestMenuWidgets()
{
    std::puts("whole screen: the pause-family widgets, the slot menus and the full-screen Blueprint menus give their reasons");
    auto signals        = Readable();
    signals.pauseWidget = true;  // a Blueprint-opened pause mode: no state 8
    EXPECT(ReasonOf(signals) == WholeScreenReason::PauseWidget);

    for (const std::uint16_t menu : {lss::views::kJukebox, lss::views::kCarriage, lss::views::kPointWarp,
                                     lss::views::kCardDeckNpc, lss::views::kChocoboStable, lss::views::kChocoboSelect,
                                     lss::views::kChocoboRace, lss::views::kCardChallenge, lss::views::kBrawlerSelect,
                                     lss::views::kBrawlerWinner})
    {
        signals             = Readable();
        signals.nativeMenus = menu;
        EXPECT(ReasonOf(signals) == WholeScreenReason::NativeMenu);
    }

    signals              = Readable();
    signals.screenWidget = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScreenWidget);

    std::puts("whole screen: on/off 153 without the minigame bit, 92 and Chadley's state switch nothing");
    signals               = Readable();
    signals.colosseumFlag = true;
    signals.miniGameMenu  = true;
    for (const std::int8_t state : {std::int8_t{-1}, std::int8_t{0}, std::int8_t{1}, std::int8_t{2}, std::int8_t{3},
                                    std::int8_t{4}, std::int8_t{5}})
    {
        signals.chadleyState = state;
        EXPECT(ReasonOf(signals) == WholeScreenReason::None);
    }

    std::puts("whole screen: the state machine and the on/off menus are named before the widget reads");
    signals             = Readable();
    signals.gameState   = lss::views::kStatePauseMenu;
    signals.pauseWidget = true;  // the state-8 openers create the same widgets
    EXPECT(ReasonOf(signals) == WholeScreenReason::PauseMenu);
    signals.gameState   = 6;
    signals.scriptMenu  = true;
    signals.nativeMenus = lss::views::kJukebox;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScriptMenu);
    signals.scriptMenu  = false;
    signals.pauseWidget = false;
    signals.screenWidget = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::NativeMenu);
    signals.nativeMenus = 0;
    signals.cutscene    = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScreenWidget);
}

void TestMinigames()
{
    using lss::views::kExclusionMiniGame;
    using lss::views::kExclusionSecond;
    std::puts("whole screen: a minigame's on/off 36 holds single for the whole game; the field uses stay split");

    // Frog jumping, Run Wild, the piano, the shooting gallery (live 2026-10-16): 36 is held while the bits flash.
    auto play            = Readable();
    play.minigamePlaying = true;
    for (const std::uint32_t mask : {0u, 1u, kExclusionSecond, kExclusionMiniGame | 1u, kExclusionMiniGame})
    {
        play.exclusion = mask;
        EXPECT(ReasonOf(play) == WholeScreenReason::Minigame);
    }
    play.inBattle = true;  // 36 needs no battle guard (no battle holds it; the cactus mission is a battle with 140)
    EXPECT(ReasonOf(play) == WholeScreenReason::Minigame);

    // The chicken lure, the chocobo capture's chase: 36 held by a field use.
    auto field            = Readable();
    field.minigamePlaying = true;
    field.fieldMinigame   = true;
    EXPECT(ReasonOf(field) == WholeScreenReason::None);
    field.exclusion = kExclusionSecond;  // the lure's result, a field event's bit 1: still split
    EXPECT(ReasonOf(field) == WholeScreenReason::None);
    field.exclusion = kExclusionMiniGame;  // the chocobo capture's catch step: the minigame bit, a stage
    EXPECT(ReasonOf(field) == WholeScreenReason::Minigame);
    field.inBattle = true;  // ... but not in a battle
    EXPECT(ReasonOf(field) == WholeScreenReason::None);

    auto ids        = Readable();  // the crash box's 139, the cactus mission's 140 (their bit 1 is cleared for play)
    ids.minigameIds = true;
    ids.inBattle    = true;
    EXPECT(ReasonOf(ids) == WholeScreenReason::Minigame);

    auto frog     = Readable();  // the frog manager in a game state: single even if 36 is not read
    frog.frogGuys = true;
    EXPECT(ReasonOf(frog) == WholeScreenReason::Minigame);

    auto entry      = Readable();  // an entry prompt, or a native flow's first states before its 36: the minigame bit
    entry.exclusion = kExclusionMiniGame;
    EXPECT(ReasonOf(entry) == WholeScreenReason::Minigame);

    auto battle      = Readable();  // the minigame bit during a battle (a VR summon intro, a battle's own use)
    battle.exclusion = kExclusionMiniGame;
    battle.inBattle  = true;
    EXPECT(ReasonOf(battle) == WholeScreenReason::None);

    auto vr          = Readable();  // a VR / colosseum session's menu or result: the minigame bit under on/off 153
    vr.exclusion     = kExclusionMiniGame;
    vr.colosseumFlag = true;
    EXPECT(ReasonOf(vr) == WholeScreenReason::ArenaMenu);
    vr.inBattle = true;  // its intro runs inside the battle: split
    EXPECT(ReasonOf(vr) == WholeScreenReason::None);
    vr.exclusion = 0;  // the fight: the bit is cleared
    EXPECT(ReasonOf(vr) == WholeScreenReason::None);
    vr.inBattle = false;  // between phases with the bit clear: split
    EXPECT(ReasonOf(vr) == WholeScreenReason::None);

    auto summonBoss      = Readable();  // RestrictBattleFieldBySummonBoss: the second bit in battle, no minigame
    summonBoss.exclusion = kExclusionSecond;
    summonBoss.inBattle  = true;
    EXPECT(ReasonOf(summonBoss) == WholeScreenReason::None);

    auto second      = Readable();  // program actions, summon scenes, data events: the second bit alone
    second.exclusion = kExclusionSecond;
    EXPECT(ReasonOf(second) == WholeScreenReason::None);

    auto gbike  = Readable();  // G-Bike sets no bit: its manager, or a minigame-only map
    gbike.gbike = true;
    EXPECT(ReasonOf(gbike) == WholeScreenReason::Minigame);
    auto map        = Readable();
    map.minigameMap = true;
    map.inBattle    = true;
    EXPECT(ReasonOf(map) == WholeScreenReason::Minigame);

    auto other      = Readable();  // events, travel and battle exits use other bits
    other.exclusion = ~kExclusionMiniGame;
    EXPECT(ReasonOf(other) == WholeScreenReason::None);

    auto mixed            = Readable();  // a field use's 36 does not hide a stage's own signals
    mixed.minigamePlaying = true;
    mixed.fieldMinigame   = true;
    mixed.minigameIds     = true;
    EXPECT(ReasonOf(mixed) == WholeScreenReason::Minigame);
    mixed.minigameIds = false;
    mixed.frogGuys    = true;
    EXPECT(ReasonOf(mixed) == WholeScreenReason::Minigame);

    auto session            = Readable();  // 36 under on/off 153 (no session holds it; S-HELDBITS): the session decides
    session.minigamePlaying = true;
    session.colosseumFlag   = true;
    session.exclusion       = kExclusionMiniGame;
    EXPECT(ReasonOf(session) == WholeScreenReason::ArenaMenu);
    session.exclusion = 0;
    EXPECT(ReasonOf(session) == WholeScreenReason::Minigame);
    session.minigamePlaying = false;
    session.exclusion       = kExclusionMiniGame;
    session.inBattle        = true;
    EXPECT(ReasonOf(session) == WholeScreenReason::None);

    std::puts("whole screen: a minigame is named after the menus and before cutscenes");
    auto paused            = Readable();
    paused.minigamePlaying = true;
    paused.gameState       = lss::views::kStatePauseMenu;
    EXPECT(ReasonOf(paused) == WholeScreenReason::PauseMenu);
    play.inBattle    = false;
    play.nativeMenus = lss::views::kChocoboRace;
    EXPECT(ReasonOf(play) == WholeScreenReason::NativeMenu);
    play.nativeMenus = 0;
    play.cutscene    = true;
    EXPECT(ReasonOf(play) == WholeScreenReason::Minigame);
}

void TestFieldUseNames()
{
    using lss::views::FieldUseName;
    std::puts("whole screen: on/off 36's field uses by name (the two native flows, exactly)");
    EXPECT(FieldUseName(L"FEndChickenLureMiniGameFlow"));
    EXPECT(FieldUseName(L"EndChocoboCaptureManager"));
    EXPECT(!FieldUseName(L"FEndChickenLureMiniGameFlowX"));  // the flows by exact name
    EXPECT(!FieldUseName(L"act2360_MiniGame_Buggy"));  // the desert buggy course: a minigame
    EXPECT(!FieldUseName(L"act2360_MiniGame_Buggy_Hard"));
    EXPECT(!FieldUseName(L"act3110_Gravity_010_A_Room"));  // the gravity console holds the party: single
    EXPECT(!FieldUseName(L"act3110_Other"));
    EXPECT(!FieldUseName(L"act1310_MiniGame_FrogGuys_Normal_Quest"));
    EXPECT(!FieldUseName(L"act2110_Piano_MainMode"));
    EXPECT(!FieldUseName(L"act2100_RunWild_Battle"));
    EXPECT(!FieldUseName(L""));
    EXPECT(!FieldUseName(L"?"));
}

void TestSceneLeadIn()
{
    std::puts("whole screen: a skippable scene's lead-in holds the single view until its cinematic takes over");
    auto signals      = Readable();
    signals.skippable = true;  // the dialogue is skippable, no cinematic yet
    EXPECT(ReasonOf(signals) == WholeScreenReason::SkippableScene);
    signals.cutscene = true;  // the cinematic starts under it
    EXPECT(ReasonOf(signals) == WholeScreenReason::Cutscene);
    signals.skippable = false;
    signals.cutscene  = false;
    signals.skipping  = true;  // skipped: the game holds black
    EXPECT(ReasonOf(signals) == WholeScreenReason::CutsceneSkip);
}

void TestOrder()
{
    std::puts("whole screen: the pause-menu state is named first when several hold");
    auto signals       = Readable();
    signals.gameState  = lss::views::kStatePauseMenu;
    signals.scriptMenu = true;
    signals.cutscene   = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::PauseMenu);
    signals.gameState = 6;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScriptMenu);

    signals          = Readable();
    signals.jobBoard = true;
    signals.cutscene = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::JobBoard);
    signals.scriptMenu = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::ScriptMenu);

    signals       = Readable();
    signals.title = true;
    signals.skipping = true;
    EXPECT(ReasonOf(signals) == WholeScreenReason::CutsceneSkip);  // the title is named only when nothing else holds
    signals.skipping  = false;
    signals.gameState = lss::views::kStatePauseMenu;
    EXPECT(ReasonOf(signals) == WholeScreenReason::PauseMenu);
}

void TestOtherStates()
{
    std::puts("whole screen: other game states give no reason");
    constexpr std::uint8_t kOthers[] = {0, 1, 5, 6, 9, 11};
    for (const std::uint8_t state : kOthers)
    {
        auto signals      = Readable();
        signals.gameState = state;
        EXPECT(ReasonOf(signals) == WholeScreenReason::None);
    }
}

}  // namespace

int main()
{
    TestNothingKnown();
    TestEachSignal();
    TestMenuWidgets();
    TestMinigames();
    TestFieldUseNames();
    TestSceneLeadIn();
    TestOrder();
    TestOtherStates();

    return lss::test::Report();
}
