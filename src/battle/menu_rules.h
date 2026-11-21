#pragma once

// The command menu's pure rules (battle/command_menu_tick.h, command_menu.h), tested on the host
// (tests/menu_rules_test.cpp). The modules keep the engine reads and the redirects. The rules
// over the menus (the world slow, the wait sound) take every player's, by seat (0 player 1), in the order the menus
// tick: player 1's, then each seat's.

#include <algorithm>
#include <array>
#include <cstdint>

namespace lss::battle::menu_rules {

// ---- a seat's pad's route into the seat's menu (command_menu_tick) ----

/// A seat's press runs in the seat's UI while the seat's controller ticks (in battle, and outside it where the seat's
/// menu opens outside battle), with the seat's menu layer in place and player 1's top layer the command menu's own (a
/// pause menu or popup keeps the seat's pad out of every menu). The game's own gates on the seat's controller decide the
/// rest: its battle input in battle, its field menu and menu-allowed bytes outside it.
constexpr bool PressAllowed(bool seatTicks, std::uint8_t menuLayerSeat, std::uint8_t menuLayerOne,
                            bool playerOneTopIsCommandMenu) noexcept
{
    return seatTicks && menuLayerSeat != 0 && menuLayerOne != 0 && playerOneTopIsCommandMenu;
}

/// A seat's press runs with the seat's target fields in battle: the menu's target-switch handler writes them, so the
/// seat's are in or the press does not run. A release writes none, and outside battle the game holds none to switch.
constexpr bool PressNeedsTargets(bool press, bool inBattle) noexcept
{
    return press && inBattle;
}

/// A seat's pad's keys whose press went to the seat's UI, so their release goes there too (one set per seat: two seats
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

    /// True (and forgotten) if `key`'s press went to the seat's UI.
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

/// One menu's world-slow value as a later pass sees it: whether it came this frame, and the value.
struct SlowValue
{
    bool  thisFrame = false;
    float value     = 1.0F;
};

/// The world-slow value a tick writes while no menu waits: full speed.
inline constexpr float kFullSpeed = 1.0F;

/// The value a menu asks for under the slow-down switch: the game's own with the switch on, full speed with it off (an
/// open menu then leaves the world running).
constexpr float SlowAsked(bool slows, float value) noexcept
{
    return slows ? value : kFullSpeed;
}

/// The world-slow value written in a seat's pass, over the menus that ticked before it (player 1's, then the earlier
/// seats'): any menu slows the world (policy A), so the smallest of its own and those that came this frame.
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

// ---- the slow-down switch's grey and muffle (menu_tint) ----
// With the switch off a menu's wait raises neither: its grey and its muffle are skipped at the wait's open, and its
// close finds nothing to take down. A switch while menus wait settles what they hold, once, with the game's own calls.

/// A wait state that has raised the grey and the muffle and has not begun to close (opening 1, open 5).
constexpr bool Waiting(std::int32_t wait) noexcept
{
    return wait == 1 || wait == 5;
}

enum class Flip : std::uint8_t
{
    Nothing,
    Raise,  // the game's own start (the grey's enable or count, the muffle's on)
    Lift,   // the game's own stop (the grey's disable or count back, the muffle's off)
};

/// One menu's grey at a switch: `held` is whether it holds the grey (player 1's the latch, a seat's its count).
/// Switched off, a held grey is taken down; switched on, a waiting menu without its grey raises it.
constexpr Flip GreyAtSwitch(bool slows, std::int32_t wait, bool held) noexcept
{
    if (!slows)
    {
        return held ? Flip::Lift : Flip::Nothing;
    }
    return Waiting(wait) && !held ? Flip::Raise : Flip::Nothing;
}

/// The one muffle at a switch, over every menu's wait state (`players` of them). Switched off: lifted if any menu has
/// raised it, a closing one too, since its own close is then skipped. Switched on: raised if any menu still waits, and
/// its close then lifts it.
constexpr Flip SoundAtSwitch(bool slows, const std::int32_t* waits, int players) noexcept
{
    for (int i = 0; i < players; ++i)
    {
        if (slows ? Waiting(waits[i]) : waits[i] != 0)
        {
            return slows ? Flip::Raise : Flip::Lift;
        }
    }
    return Flip::Nothing;
}

// ---- a seat's controller following the game's (command_menu) ----

/// A refused join Setup (the seat's focus scope could not be entered) is a wait, never a give-up: the first kSetupTries
/// tries run on consecutive ticks, later ones every kWaitGap ticks, until one runs.
inline constexpr int kSetupTries = 5;
inline constexpr int kWaitGap    = 30;

/// Whether a join Setup is tried this tick; `waitTicks` counts the ticks between paced tries.
constexpr bool JoinTryDue(int tries, unsigned& waitTicks) noexcept
{
    return tries < kSetupTries || ++waitTicks % kWaitGap == 0;
}

/// The game's controller is set up and no Setup of its own is pending or deferred: a seat's may follow it.
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

}  // namespace lss::battle::menu_rules
