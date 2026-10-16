#include "views/whole_screen.h"

#include <array>

namespace lss::views {

namespace {

// On/off 36's setter is the FName of the last SetOn that passed one: the event runner passes the action's own name (its
// Action row), the native flows their class name. Every other 36 holder in the game's Action tables is a minigame or a
// minigame's scene (RE-p2-minigames-stage2.md, S-STAGEMG2's list of all 409).
constexpr std::array<std::wstring_view, 2> kFieldFlows{L"FEndChickenLureMiniGameFlow", L"EndChocoboCaptureManager"};
// The Temple's gravity rooms (act3110_Gravity_*) are not a field use: their console floats the whole party until player 1
// chooses (the user's call, 2026-10-16), so they go single like the other 36 holders.
constexpr std::array<std::wstring_view, 1> kFieldActions{L"act2360_MiniGame_Buggy"};

}  // namespace

bool FieldUseName(std::wstring_view setter) noexcept
{
    for (const std::wstring_view name : kFieldFlows)
    {
        if (setter == name)
        {
            return true;
        }
    }
    for (const std::wstring_view prefix : kFieldActions)
    {
        if (setter.starts_with(prefix))
        {
            return true;
        }
    }
    return false;
}

bool ArenaMenu(const ScreenSignals& signals) noexcept
{
    return signals.colosseumFlag && (signals.exclusion & kExclusionMiniGame) != 0 && !signals.inBattle;
}

bool MinigameStage(const ScreenSignals& signals) noexcept
{
    if (signals.gbike || signals.minigameMap)
    {
        return true;
    }
    // 36 is held for the whole game (the bits flash); a field use's 36 is not a minigame stage. 36 needs neither the
    // battle nor the colosseum guard: no battle or VR / colosseum session holds it (S-HELDBITS).
    if ((signals.minigamePlaying && !signals.fieldMinigame) || signals.minigameIds || signals.frogGuys)
    {
        return true;
    }
    return (signals.exclusion & kExclusionMiniGame) != 0 && !signals.colosseumFlag && !signals.inBattle;
}

WholeScreenReason ReasonOf(const ScreenSignals& signals) noexcept
{
    if (!signals.readable)
    {
        return WholeScreenReason::None;
    }
    if (signals.gameState == kStatePauseMenu)
    {
        return WholeScreenReason::PauseMenu;
    }
    if (signals.gameState == kStateCinemaPause)
    {
        return WholeScreenReason::CinemaPause;
    }
    if (signals.scriptMenu)
    {
        return WholeScreenReason::ScriptMenu;
    }
    if (signals.jobBoard)
    {
        return WholeScreenReason::JobBoard;
    }
    if (signals.pauseWidget)
    {
        return WholeScreenReason::PauseWidget;
    }
    if (signals.nativeMenus != 0)
    {
        return WholeScreenReason::NativeMenu;
    }
    if (signals.screenWidget)
    {
        return WholeScreenReason::ScreenWidget;
    }
    if (ArenaMenu(signals))
    {
        return WholeScreenReason::ArenaMenu;
    }
    if (MinigameStage(signals))
    {
        return WholeScreenReason::Minigame;
    }
    if (signals.cutscene)
    {
        return WholeScreenReason::Cutscene;
    }
    if (signals.skippable)
    {
        return WholeScreenReason::SkippableScene;
    }
    if (signals.skipping)
    {
        return WholeScreenReason::CutsceneSkip;
    }
    return signals.title ? WholeScreenReason::Title : WholeScreenReason::None;
}

const wchar_t* NameOf(WholeScreenReason reason) noexcept
{
    switch (reason)
    {
        case WholeScreenReason::None: return L"none";
        case WholeScreenReason::PauseMenu: return L"a pause menu (EndGameState 8)";
        case WholeScreenReason::CinemaPause: return L"the cutscene pause (EndGameState 7)";
        case WholeScreenReason::ScriptMenu: return L"a script-opened menu (on/off table)";
        case WholeScreenReason::JobBoard: return L"the job board";
        case WholeScreenReason::PauseWidget: return L"a pause-family menu (IsPauseMenuClosed)";
        case WholeScreenReason::NativeMenu: return L"a script menu with its own UI slot";
        case WholeScreenReason::ScreenWidget: return L"a full-screen Blueprint menu";
        case WholeScreenReason::ArenaMenu: return L"a VR or colosseum menu or result (on/off 153, the minigame bit, not in battle)";
        case WholeScreenReason::Minigame: return L"a minigame on its own stage";
        case WholeScreenReason::Cutscene: return L"a cutscene";
        case WholeScreenReason::SkippableScene: return L"a skippable scene (on/off 12)";
        case WholeScreenReason::CutsceneSkip: return L"a cutscene skip";
        case WholeScreenReason::Title: return L"the title";
    }
    return L"?";
}

}  // namespace lss::views
