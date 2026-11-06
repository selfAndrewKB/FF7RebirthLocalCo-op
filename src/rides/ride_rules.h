#pragma once

// The rides' pure decisions (rides/chocobo_ride.h, wheelie_ride.h, shared_ride.h), tested on the host
// (tests/ride_rules_test.cpp). The modules keep the engine reads, the scopes and the log. The same rules serve every
// seat; a text that names the rider takes the seat.

#include "platform/angles.h"
#include "players/seats.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace lss::rides::ride_rules {

// ---- chocobo ----

/// The game's end of the seat's coordinator waits for it to reach riding: `tries` more ticks, unless forced.
constexpr bool EndRetryAllowed(bool force, std::uint32_t& tries, std::uint32_t limit) noexcept
{
    return !force && ++tries < limit;
}

enum class Seated
{
    Unridden,  // the chocobo's ridden flag is clear: that ride is over (a held-back chocobo may be boarded again)
    HeldBack,  // not to be boarded again until its ridden flag clears
    Detached,  // the member is not attached
    Battle,
    Seated,    // taken
};

/// Whether the member seated on her chocobo is taken (after the manager says she rides and her chocobo was found).
constexpr Seated SeatedVerdict(bool ridden, bool heldBack, bool attached, bool inBattle) noexcept
{
    if (!ridden)
    {
        return Seated::Unridden;
    }
    if (heldBack)
    {
        return Seated::HeldBack;
    }
    if (!attached)
    {
        return Seated::Detached;
    }
    return inBattle ? Seated::Battle : Seated::Seated;
}

/// What the ride's tick reads of it (the rest is read only while the chocobo is still there).
struct EndReads
{
    bool gone        = false;  // the chocobo's weak pointer no longer names it
    bool ridden      = true;   // its ridden flag
    bool inBattle    = false;
    bool entryRiding = true;   // the ride manager's entry for the member
    bool attached    = true;
    bool holds       = true;   // the seat's controller holds the chocobo and the chocobo names it back
};

enum class EndReason
{
    None,
    Gone,
    Dismounted,  // the game dismounted the party: wait for the getoff
    Battle,
    ManagerEnded,
    Detached,
    LostPossession,
};

constexpr EndReason EndReasonOf(const EndReads& reads) noexcept
{
    if (reads.gone)
    {
        return EndReason::Gone;
    }
    if (!reads.ridden)
    {
        return EndReason::Dismounted;
    }
    if (reads.inBattle)
    {
        return EndReason::Battle;
    }
    if (!reads.entryRiding)
    {
        return EndReason::ManagerEnded;
    }
    if (!reads.attached)
    {
        return EndReason::Detached;
    }
    return reads.holds ? EndReason::None : EndReason::LostPossession;
}

/// The reason's text for the log (null for None), player 2's ride's.
constexpr const wchar_t* Describe(EndReason reason) noexcept
{
    switch (reason)
    {
        case EndReason::None: return nullptr;
        case EndReason::Gone: return L"the chocobo is gone";
        case EndReason::Dismounted: return L"the game dismounted the party";
        case EndReason::Battle: return L"a battle started";
        case EndReason::ManagerEnded: return L"the ride manager ended her ride";
        case EndReason::Detached: return L"the member was detached";
        case EndReason::LostPossession: return L"the rider's controller no longer holds the chocobo";
    }
    return nullptr;
}

/// The reason's text for `seat`'s ride (null for None): Describe's, with the lost possession naming the seat's
/// controller. A literal: the ride keeps the pointer.
constexpr const wchar_t* Describe(EndReason reason, int seat) noexcept
{
    if (reason != EndReason::LostPossession || seat == players::kFirstSeat)
    {
        return Describe(reason);
    }
    constexpr const wchar_t* kLost[players::kMaxPlayers] = {
        L"player 1's controller no longer holds the chocobo", L"player 2's controller no longer holds the chocobo",
        L"player 3's controller no longer holds the chocobo"};
    return seat >= 0 && seat < players::kMaxPlayers ? kLost[seat] : L"the rider's controller no longer holds the chocobo";
}

/// The game's getoff animation is over: she is detached, a battle started, the animation's time ran out, or the
/// chocobo is gone.
constexpr bool GetOffDone(bool attached, bool inBattle, bool timedOut, bool gone) noexcept
{
    return !attached || inBattle || timedOut || gone;
}

// ---- wheelie ----

enum class ForcedGetOff
{
    Skip,           // not a scooter getoff, or the seat's coordinator is not riding (the game ends rides only then)
    Follow,         // the game's getoff, as it came
    FollowInstant,  // made instant: player 1 is far enough for the rental-return warp to take the scooter home
};

/// `farOff()` is read only when the plan needs it.
template <class FarFn>
ForcedGetOff ForcedGetOffPlanOf(std::uint8_t type, std::int32_t state, std::uint8_t scooterKey, std::int32_t riding,
                                bool instantAlready, FarFn&& farOff) noexcept  // not `far`: a Windows macro
{
    if ((type != 0 && type != scooterKey) || state != riding)
    {
        return ForcedGetOff::Skip;
    }
    return !instantAlready && farOff() ? ForcedGetOff::FollowInstant : ForcedGetOff::Follow;
}

enum class CoordChange
{
    None,      // the same state, or another state the tick does not act on
    Finished,  // idle: the game's getoff finished
    Gone,      // a negative state: the coordinator is gone; rides stop for the session
};

constexpr CoordChange CoordChangeOf(std::int32_t was, std::int32_t now, std::int32_t idle) noexcept
{
    if (now == was)
    {
        return CoordChange::None;
    }
    if (now == idle)
    {
        return CoordChange::Finished;
    }
    return now < 0 ? CoordChange::Gone : CoordChange::None;
}

/// FieldSummonBody's test: a move request carries a stick only when it is not (nearly) zero.
inline bool RequestMoves(const float (&request)[3], float epsilon) noexcept
{
    return !(std::fabs(request[0]) <= epsilon && std::fabs(request[1]) <= epsilon && std::fabs(request[2]) <= epsilon);
}

/// The stick pair turned by the primary camera's yaw (degrees), as FieldSummonBody computes the world stick.
inline std::array<float, 2> WorldStickOf(float stickX, float stickY, float yawDegrees) noexcept
{
    const float yaw = yawDegrees * platform::kRadiansPerDegree;
    return {stickY * std::cos(yaw) - stickX * std::sin(yaw), stickY * std::sin(yaw) + stickX * std::cos(yaw)};
}

// ---- a shared ride (player 1 on the buggy or the bronco) ----

/// Player 1 rides a shared vehicle: its coordinator is one of the known kinds and not idle (a bronco coordinator still
/// set at state 0 is not a ride).
constexpr bool SharedRiding(bool kindKnown, std::int32_t state, std::int32_t idle) noexcept
{
    return kindKnown && state != idle;
}

/// The ride started, ended, or switched vehicle (ended then started).
constexpr bool RideChanged(bool wasRiding, std::size_t wasKind, bool riding, std::size_t kind) noexcept
{
    return riding != wasRiding || (riding && kind != wasKind);
}

}  // namespace lss::rides::ride_rules
