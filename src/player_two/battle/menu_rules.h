#pragma once

// The command menu's pure rules (player_two/battle/command_menu_tick.h, command_menu_two.h), tested on the host
// (tests/menu_rules_test.cpp). The modules keep the engine reads and the redirects. Written for player 2; the rules
// over the menus (the world slow, the wait sound) take every player's, by seat (0 player 1), in the order the menus
// tick: player 1's, then each seat's.

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

/// Pad 2's keys whose press went to player 2's UI, so their release goes there too (one set per seat: two seats
/// holding the same action keep their own). A pad has fewer inputs (buttons, triggers, stick clicks and stick
/// directions) than the capacity, so every held key fits.
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

/// One menu's world-slow value as a later pass sees it: whether it came this frame, and the value.
struct SlowValue
{
    bool  thisFrame = false;
    float value     = 1.0F;
};

/// The world-slow value written in a seat's pass, over the menus that ticked before it (player 1's, then the earlier
/// seats'): either menu slows the world (policy A), so the smallest of its own and those that came this frame. With
/// one earlier menu, SlowWritten.
constexpr float SlowWrittenAmong(const SlowValue* earlier, int count, float mine) noexcept
{
    float written = mine;
    for (int i = 0; i < count; ++i)
    {
        if (earlier[i].thisFrame)
        {
            written = std::min(earlier[i].value, written);
        }
    }
    return written;
}

/// The wait mode's sound off is skipped while another menu is still in wait mode. The menus tick in order (player 1's
/// first, then each seat's): one that ticks later still counts while closing (4), so if several close in one frame the
/// last one plays it. `me`: the ticking menu's player (0 player 1, s a seat; negative outside a menu's tick); `waits`:
/// every player's wait state, by seat, `players` of them.
constexpr bool SkipWaitSoundOffAmong(int me, const std::int32_t* waits, int players) noexcept
{
    if (me < 0)
    {
        return false;
    }
    for (int other = 0; other < players; ++other)
    {
        const std::int32_t wait = waits[other];
        if (other != me && (wait == 1 || wait == 5 || (other > me && wait == 4)))
        {
            return true;
        }
    }
    return false;
}

/// The two-player rule by pass (1 player 1's tick, 2 player 2's): SkipWaitSoundOffAmong over player 1 and player 2.
constexpr bool SkipWaitSoundOff(int pass, std::int32_t waitOne, std::int32_t waitTwo) noexcept
{
    const std::int32_t waits[2] = {waitOne, waitTwo};
    return SkipWaitSoundOffAmong(pass == 1 ? 0 : pass == 2 ? 1 : -1, waits, 2);
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

/// The game's call may be made again on a seat's controller: it is built, we are on the game thread, the call was made
/// on the game's controller (the UI system's slot names it; "ours" is any seat's controller) and, when the call needs
/// the seat's member, the seat plays.
constexpr bool CanFollow(bool installed, bool failed, bool built, bool onGameThread, bool needsPlayer, bool seatPlays,
                         bool gameControllerKnown, bool gameControllerIsOurs, bool namedByUiSystem) noexcept
{
    return installed && !failed && built && onGameThread && (!needsPlayer || seatPlays) && gameControllerKnown &&
           !gameControllerIsOurs && namedByUiSystem;
}

}  // namespace lss::player_two::menu_rules
