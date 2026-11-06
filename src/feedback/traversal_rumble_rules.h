#pragma once

// The rumble list's pure rules (feedback/traversal_rumble.h), tested on the host
// (tests/traversal_rumble_rules_test.cpp). The module keeps the lists, the lock, the engine calls and the log. The
// rules call a seat's list the seat's (Route::Seat, ListTick::Seat): each seat's own list, whichever seat the add,
// stop or tick belongs to (SeatOfAdd, ListTickFor).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::feedback::traversal_rumble_rules {

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

/// An entry of player 1's list that a seat's list takes at a shared ride's start: a loop that is not ending and not
/// in states 6 or 7 (the caller skips keys the seat's list already has).
constexpr bool SeedsLoop(std::uint8_t flags, std::uint8_t state) noexcept
{
    return (flags & kFlagLoop) != 0 && (flags & kFlagEnding) == 0 && state != 6 && state != 7;
}

// ---- where an add or a stop goes ----

enum class Route
{
    Seat,          // the seat's list took it
    Game,          // the game's list
    GameAndCopy,   // the game's list, copied onto the seats' (a shared ride)
    LeftOnWorker,  // a stop of a seat's on a worker: left alone (the owner's list is named only on the game thread)
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

/// An add: that seat's list when the add is a seat's and the list took it; else the game's, copied during a shared ride
/// unless it was a seat's.
constexpr Route AddRouteOf(bool further, bool added, bool copyOne) noexcept
{
    if (further && added)
    {
        return Route::Seat;
    }
    return !further && copyOne ? Route::GameAndCopy : Route::Game;
}

/// A stop: left when a seat's source stops on a worker while the seat has a list; the seat's list when it is a
/// seat's, on the game thread, and the list stopped it; else the game's, copied during a shared ride unless it was
/// a seat's.
constexpr Route StopRouteOf(bool owner, bool gameThread, bool hasList, bool further, bool stopped,
                            bool copyOne) noexcept
{
    if (owner && !gameThread && hasList)
    {
        return Route::LeftOnWorker;
    }
    if (further && gameThread && stopped)
    {
        return Route::Seat;
    }
    return !further && copyOne ? Route::GameAndCopy : Route::Game;
}

// ---- the component's list tick ----

enum class ListTick
{
    Game,     // no list of a seat's: the game's tick
    Seat,     // a seat's component ticks the seat's list
    Skip,     // a seat is the primary player: the game's list never plays on the seat's pad
    GameOne,  // player 1's tick of the game's list (counted)
};

constexpr ListTick ListTickChoice(bool hasList, bool tickOwnerIsSeat, bool primaryIsSeat) noexcept
{
    if (!hasList)
    {
        return ListTick::Game;
    }
    if (tickOwnerIsSeat)
    {
        return ListTick::Seat;
    }
    return primaryIsSeat ? ListTick::Skip : ListTick::GameOne;
}

/// The choice with the seats: a seat's component (`ownerSeat` >= 1, active) ticks the seat's list when it has one
/// (`ownerHasList`), else the game's; any other component (player 1's) ticks the game's list when some active seat has
/// a list (`anyList`), skipped while a seat is the primary player (`primaryIsSeat`).
constexpr ListTick ListTickFor(int ownerSeat, bool ownerHasList, bool anyList, bool primaryIsSeat) noexcept
{
    return ownerSeat >= 1 ? ListTickChoice(ownerHasList, true, primaryIsSeat)
                          : ListTickChoice(anyList, false, primaryIsSeat);
}

}  // namespace lss::feedback::traversal_rumble_rules
