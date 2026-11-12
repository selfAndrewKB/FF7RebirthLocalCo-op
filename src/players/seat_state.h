#pragma once

// A seat's state in one word, derived from the session's record of the seat. The session keeps flags for its stages;
// the frame the modules read, the console's readings and the log take the word. Pure, host-tested
// (tests/seat_state_test.cpp).
//
// Of the words below, "handed" is the seat staying in without a member until the game frees one (the game's party has
// not freed it yet, or player 1 rides a vehicle the whole party shares): kept on the monitor row, or on one monitor for
// a ride and for the short grace before a waiting player leaves to wait outside (session/session_rules.h WaitOutside),
// when the seat is Awaiting. A script's hand of the member to its AI keeps the seat's member and view (the session's
// RidesAlong): "in".

namespace lss::players {

enum class SeatState
{
    Out,        // no player in the seat
    Awaiting,   // asked for; out until a party member is free for it
    Rejoining,  // taken out by a map load; joins again once the new map is ready
    In,         // in without a member of its own: joining, changing members, leaving, or none to be taken this time
    Playing,    // plays its party member
    Handed,     // in without its member while the game has it (a story script, a shared ride); the seat is kept
};

/// The session's flags for one seat, as its record keeps them.
struct SeatFlags
{
    bool in            = false;  // its local player is listed by the engine
    bool playsMember   = false;  // past the take and driving its member
    bool handed        = false;  // in without a member, waiting on the game for it (the slot's, or a shared ride)
    bool awaiting      = false;  // out until a party member is free
    bool rejoinPending = false;  // taken out by a map load, joins again once the new map is ready
};

/// One word for the flags. Out of the game: a wait for a member first, then a pending rejoin, else out. In the game:
/// playing with the member, handed while the game has it, else in.
constexpr SeatState StateOf(const SeatFlags& f) noexcept
{
    if (!f.in)
    {
        return f.awaiting ? SeatState::Awaiting : f.rejoinPending ? SeatState::Rejoining : SeatState::Out;
    }
    if (f.playsMember)
    {
        return SeatState::Playing;
    }
    return f.handed ? SeatState::Handed : SeatState::In;
}

/// The seat's local player is in the game (in, playing or handed).
constexpr bool InGame(SeatState state) noexcept
{
    return state == SeatState::In || state == SeatState::Playing || state == SeatState::Handed;
}

/// The reading's word.
constexpr const wchar_t* StateName(SeatState state) noexcept
{
    switch (state)
    {
        case SeatState::Out: return L"out";
        case SeatState::Awaiting: return L"awaiting";
        case SeatState::Rejoining: return L"rejoining";
        case SeatState::In: return L"in";
        case SeatState::Playing: return L"playing";
        case SeatState::Handed: return L"handed";
    }
    return L"?";
}

/// What the word means for the player, for the log line of a change.
constexpr const wchar_t* StateMeaning(SeatState state) noexcept
{
    switch (state)
    {
        case SeatState::Out: return L"out of the game";
        case SeatState::Awaiting: return L"waiting outside the game for a free party member";
        case SeatState::Rejoining: return L"waiting to join again once the new map is ready";
        case SeatState::In: return L"in the game without a party member";
        case SeatState::Playing: return L"playing their party member";
        case SeatState::Handed: return L"in the game while the game has their party member";
    }
    return L"?";
}

}  // namespace lss::players
