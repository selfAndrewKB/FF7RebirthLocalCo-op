#pragma once

// The rumble list's pure rules (feedback/rumble_list_two.h), tested on the host
// (tests/rumble_rules_test.cpp). The module keeps the lists, the lock, the engine calls and the log. The rules name a
// seat's list as "player 2's" (Route::Two, ListTick::Two): each seat's own list, whichever seat the add, stop or tick
// belongs to (SeatOfAdd, ListTickFor).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::feedback::rumble_rules {

/// A small set of keys with a fixed capacity: the shared ride's copied keys, kept until their stop is copied too.
template <std::size_t N>
class KeySet
{
public:
    /// True if `key` was added (not present, room left).
    bool Insert(std::uint64_t key) noexcept
    {
        const auto end = keys_.begin() + count_;
        if (std::find(keys_.begin(), end, key) != end || count_ >= keys_.size())
        {
            return false;
        }
        keys_[count_++] = key;
        return true;
    }

    /// True if `key` was there (the last key takes its place).
    bool Erase(std::uint64_t key) noexcept
    {
        const auto end = keys_.begin() + count_;
        const auto at  = std::find(keys_.begin(), end, key);
        if (at == end)
        {
            return false;
        }
        *at = keys_[--count_];
        return true;
    }

    void Clear() noexcept { count_ = 0; }

    std::size_t                        Count() const noexcept { return count_; }
    const std::array<std::uint64_t, N>& Keys() const noexcept { return keys_; }  // the first Count() are live

private:
    std::array<std::uint64_t, N> keys_{};
    std::size_t                  count_ = 0;
};

// ---- the shared ride's seed of player 1's loops ----

inline constexpr std::uint8_t kFlagLoop   = 0x02;
inline constexpr std::uint8_t kFlagEnding = 0x04 | 0x08 | 0x10;

/// An entry of player 1's list that player 2's list takes at a shared ride's start: a loop that is not ending and not
/// in states 6 or 7 (the caller skips keys player 2's list already has).
constexpr bool SeedsLoop(std::uint8_t flags, std::uint8_t state) noexcept
{
    return (flags & kFlagLoop) != 0 && (flags & kFlagEnding) == 0 && state != 6 && state != 7;
}

// ---- where an add or a stop goes ----

enum class Route
{
    Two,           // player 2's list took it
    Game,          // the game's list
    GameAndCopy,   // the game's list, copied onto player 2's (a shared ride)
    LeftOnWorker,  // a stop of player 2's on a worker: left alone (the owner's list is named only on the game thread)
};

/// Whose add or stop it is (a seat, or 0 for the game's list): the seat its source names (`ownerSeat`, any thread),
/// else on the game thread the seat that is the primary player (`primarySeat`, 0 for player 1).
constexpr int SeatOfAdd(int ownerSeat, bool gameThread, int primarySeat) noexcept
{
    if (ownerSeat >= 1)
    {
        return ownerSeat;
    }
    return gameThread && primarySeat >= 1 ? primarySeat : 0;
}

/// An add: player 2's list when it is player 2's and the list took it; else the game's, copied during a shared ride
/// unless it was player 2's.
constexpr Route AddRouteOf(bool two, bool added, bool copyOne) noexcept
{
    if (two && added)
    {
        return Route::Two;
    }
    return !two && copyOne ? Route::GameAndCopy : Route::Game;
}

/// A stop: left when player 2's source stops on a worker while player 2 has a list; player 2's list when it is player
/// 2's, on the game thread, and the list stopped it; else the game's, copied during a shared ride unless it was
/// player 2's.
constexpr Route StopRouteOf(bool owner, bool gameThread, bool hasList, bool two, bool stopped, bool copyOne) noexcept
{
    if (owner && !gameThread && hasList)
    {
        return Route::LeftOnWorker;
    }
    if (two && gameThread && stopped)
    {
        return Route::Two;
    }
    return !two && copyOne ? Route::GameAndCopy : Route::Game;
}

// ---- the component's list tick ----

enum class ListTick
{
    Game,     // no list of player 2's: the game's tick
    Two,      // player 2's component ticks player 2's list
    Skip,     // player 2 is the primary player: the game's list never plays on player 2's pad
    GameOne,  // player 1's tick of the game's list (counted)
};

constexpr ListTick ListTickChoice(bool hasList, bool tickOwnerIsTwo, bool primaryIsTwo) noexcept
{
    if (!hasList)
    {
        return ListTick::Game;
    }
    if (tickOwnerIsTwo)
    {
        return ListTick::Two;
    }
    return primaryIsTwo ? ListTick::Skip : ListTick::GameOne;
}

/// The choice with the seats: a seat's component (`ownerSeat` >= 1, active) ticks the seat's list when it has one
/// (`ownerHasList`), else the game's; any other component (player 1's) ticks the game's list when some active seat has
/// a list (`anyList`), skipped while a seat is the primary player (`primaryIsSeat`).
constexpr ListTick ListTickFor(int ownerSeat, bool ownerHasList, bool anyList, bool primaryIsSeat) noexcept
{
    return ownerSeat >= 1 ? ListTickChoice(ownerHasList, true, primaryIsSeat)
                          : ListTickChoice(anyList, false, primaryIsSeat);
}

}  // namespace lss::feedback::rumble_rules
