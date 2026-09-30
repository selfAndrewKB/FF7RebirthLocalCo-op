#include "views/whole_screen.h"

namespace lss::views {

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
        case WholeScreenReason::Cutscene: return L"a cutscene";
        case WholeScreenReason::SkippableScene: return L"a skippable scene (on/off 12)";
        case WholeScreenReason::CutsceneSkip: return L"a cutscene skip";
        case WholeScreenReason::Title: return L"the title";
    }
    return L"?";
}

}  // namespace lss::views
