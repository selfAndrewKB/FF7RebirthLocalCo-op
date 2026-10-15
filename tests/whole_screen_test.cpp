// Host-side tests for views::ReasonOf, run without the game.
//
// The cases: nothing known means no reason; each signal alone gives its reason; the pause-menu state
// wins over the others; the other game states (the field running, a load) give none.

#include "views/whole_screen.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

using lss::views::ReasonOf;
using lss::views::ScreenSignals;
using lss::views::WholeScreenReason;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

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

    std::puts("whole screen: the log-only candidates (on/off 153 and 92, Chadley's state) switch nothing");
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
    TestSceneLeadIn();
    TestOrder();
    TestOtherStates();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
