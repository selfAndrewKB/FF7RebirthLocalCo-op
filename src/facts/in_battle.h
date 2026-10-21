#pragma once

// The game's own in-battle test, found once for every module that asks it.
//
// UEndBattleAPI::IsInBattle (facts/party.h, kIsInBattle) answers whether a battle is on. Several of
// player 2's modules ask it: at their install they used to find it each for themselves and wrap the
// call each in their own guard. It is found here once, at startup before the stages, and asked
// through one guarded call; a module whose install needs the test checks that it was found.

namespace lss::platform {
class GameImage;
}

namespace lss::facts {

/// Finds the test. False (InBattle answers false, InBattleKnown false) if its bytes are not what the research read.
bool InstallInBattle(const platform::GameImage& image) noexcept;

/// The test was found: a module that needs it may install.
bool InBattleKnown() noexcept;

/// The game's answer: a battle is on. Any thread. False if the test was not found or its call faulted.
bool InBattle() noexcept;

}  // namespace lss::facts
