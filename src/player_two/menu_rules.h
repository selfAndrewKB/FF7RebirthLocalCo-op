#pragma once

// The command menu's pure rules (player_two/command_menu_tick.h, command_menu_two.h), tested on the host
// (tests/menu_rules_test.cpp). The modules keep the engine reads and the redirects.

#include <algorithm>
#include <array>
#include <cstdint>

namespace lss::player_two::menu_rules {

// ---- pad 2's route into player 2's menu (command_menu_tick) ----

/// A pad-2 press runs in player 2's UI only in battle, with player 2's menu layer in place and player 1's top layer
/// the command menu's own (a pause menu or popup keeps pad 2 out of both menus).
constexpr bool PressAllowed(std::uint8_t hudModeTwo, std::uint8_t menuLayerTwo, std::uint8_t battleInputOne,
                            std::uint8_t menuLayerOne, bool playerOneTopIsCommandMenu) noexcept
{
    return hudModeTwo != 0 && menuLayerTwo != 0 && battleInputOne != 0 && menuLayerOne != 0 && playerOneTopIsCommandMenu;
}

/// Pad 2's keys whose press went to player 2's UI, so their release goes there too. A pad has fewer inputs (buttons,
/// triggers, stick clicks and stick directions) than the capacity, so every held key fits.
class TakenKeys
{
public:
    static constexpr std::size_t kCapacity = 32;

    enum class Note
    {
        Noted,
        Already,  // noted before: nothing to do
        Full,     // more presses held than fit: a release may reach player 1's route
    };

    Note NoteTaken(std::uint64_t key) noexcept
    {
        if (std::find(keys_.begin(), keys_.end(), key) != keys_.end())
        {
            return Note::Already;
        }
        const auto free = std::find(keys_.begin(), keys_.end(), std::uint64_t{0});
        if (free == keys_.end())
        {
            return Note::Full;
        }
        *free = key;
        return Note::Noted;
    }

    /// True (and forgotten) if `key`'s press went to player 2's UI.
    bool Take(std::uint64_t key) noexcept
    {
        const auto it = std::find(keys_.begin(), keys_.end(), key);
        if (key == 0 || it == keys_.end())
        {
            return false;
        }
        *it = 0;
        return true;
    }

private:
    std::array<std::uint64_t, kCapacity> keys_{};
};

// ---- the two menus' shared engine state (command_menu_tick) ----

/// The world-slow value written in player 2's pass: either menu slows the world (policy A), so the smaller of the two
/// when player 1's came this frame.
constexpr float SlowWritten(bool playerOnesThisFrame, float playerOnes, float playerTwos) noexcept
{
    return playerOnesThisFrame ? std::min(playerOnes, playerTwos) : playerTwos;
}

/// The wait mode's sound off is skipped while the other menu is still in wait mode. Player 1's ticks first, so if both
/// close in one frame player 2's plays it.
constexpr bool SkipWaitSoundOff(int pass, std::int32_t waitOne, std::int32_t waitTwo) noexcept
{
    return (pass == 1 && (waitTwo == 1 || waitTwo == 5 || waitTwo == 4)) || (pass == 2 && (waitOne == 1 || waitOne == 5));
}

// ---- player 2's controller following the game's (command_menu_two) ----

/// A refused join Setup (player 2's focus scope could not be entered) is a wait, never a give-up: the first kSetupTries
/// tries run on consecutive ticks, later ones every kWaitGap ticks, until one runs.
inline constexpr int kSetupTries = 5;
inline constexpr int kWaitGap    = 30;

/// Whether a join Setup is tried this tick; `waitTicks` counts the ticks between paced tries.
constexpr bool JoinTryDue(int tries, unsigned& waitTicks) noexcept
{
    return tries < kSetupTries || ++waitTicks % kWaitGap == 0;
}

/// The game's controller is set up and no Setup of its own is pending or deferred: player 2's may follow it.
constexpr bool GameSetUp(std::uintptr_t gameController, std::uint8_t setupDone, std::uint8_t setupPending,
                         std::uint8_t setupDeferred) noexcept
{
    return gameController != 0 && setupDone != 0 && setupPending == 0 && setupDeferred == 0;
}

/// The game's call may be made again on player 2's controller: it is built, we are on the game thread, the call was
/// made on the game's controller (the UI system's slot names it) and, when the call needs player 2's member, player 2
/// plays.
constexpr bool CanFollow(bool installed, bool failed, bool built, bool onGameThread, bool needsPlayer, bool playerTwoPlays,
                         bool gameControllerKnown, bool gameControllerIsOurs, bool namedByUiSystem) noexcept
{
    return installed && !failed && built && onGameThread && (!needsPlayer || playerTwoPlays) && gameControllerKnown &&
           !gameControllerIsOurs && namedByUiSystem;
}

}  // namespace lss::player_two::menu_rules
