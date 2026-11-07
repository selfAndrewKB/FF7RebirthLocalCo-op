#pragma once

// The camera and world modules' pure decisions (attack_framing, location_rows, walk_limits, world_actor_reach,
// seat_switch_volumes, camera_repair, party_warp, traversal_camera, camera_distance, shoulder_scale), tested on the
// host (tests/world_rules_test.cpp). The modules keep the engine reads, the scopes and the log. The players are
// numbered by seat (players/seats.h): 0 player 1, then the seats.

#include "players/seats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lss::world::world_rules {

// ---- attack_framing: the per-sequence and per-shot latches, and the shot's verdict ----

struct Latch
{
    std::uint64_t key   = 0;
    std::uint8_t  value = 0;
    std::uint64_t used  = 0;
};

/// The latch for `key`, made (value 0) if absent; the least recently used goes when full.
template <std::size_t N>
Latch& LatchFor(std::array<Latch, N>& latches, std::uint64_t key, std::uint64_t tick, bool& made) noexcept
{
    Latch* oldest = &latches[0];
    for (Latch& latch : latches)
    {
        if (latch.key == key)
        {
            latch.used = tick;
            made       = false;
            return latch;
        }
        if (latch.used < oldest->used)
        {
            oldest = &latch;
        }
    }
    *oldest = {key, 0, tick};
    made    = true;
    return *oldest;
}

/// Whether a shot of `target` (non-null: the caller decides nothing without one) stays on the camera of `viewer` (0
/// player 1's, else a seat's). `humans[s]` is player s's character, by seat (players/seats.h; index 0 player 1's;
/// null: no character for that seat). A target that is no human's character shows on every camera (the AI ally or an
/// enemy, as solo shows it); the viewer's own target shows; another player's target is off unless this camera is the
/// viewer's own (the shot's owner is the viewer's character). True: show.
constexpr bool ShotShows(const void* target, const void* const (&humans)[players::kMaxPlayers], int viewer,
                         const void* owner) noexcept
{
    int of = -1;
    for (int seat = 0; seat < players::kMaxPlayers && of < 0; ++seat)
    {
        if (humans[seat] != nullptr && target == humans[seat])
        {
            of = seat;
        }
    }
    if (of < 0 || of == viewer || viewer < 0 || viewer >= players::kMaxPlayers)
    {
        return true;
    }
    return owner == humans[viewer];
}

// ---- location_rows: the camera location volume a seat's character is in ----

/// A volume the pick considers: one that changes the camera and is not disabled.
constexpr bool LocationCandidate(std::uint32_t flags, std::uint32_t disabledFlag, std::uint32_t changeCameraFlag) noexcept
{
    return (flags & disabledFlag) == 0 && (flags & changeCameraFlag) != 0;
}

/// The lowest CameraSettingPriority wins; a tie keeps the first found.
constexpr bool LowerPriority(std::uint8_t priority, std::uint8_t best) noexcept
{
    return priority < best;
}

// ---- walk_limits: the move-type stack the game keeps per controller ----

/// The game's stack of move-type pairs (index 0 = top): a push of a pair already on it gains a reference, a new one
/// goes to the front; a pop loses a reference and leaves at 0. `Pair` is the module's own (equality-comparable).
template <class Pair, std::size_t N>
class PairStack
{
public:
    struct Entry
    {
        int  refs = 0;
        Pair pair{};
    };

    void Push(Pair pair) noexcept
    {
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].pair == pair)
            {
                ++entries_[i].refs;
                return;
            }
        }
        for (int i = count_; i > 0; --i)
        {
            entries_[i] = entries_[i - 1];
        }
        entries_[0] = {1, pair};
        ++count_;
    }

    void Pop(Pair pair) noexcept
    {
        for (int i = 0; i < count_; ++i)
        {
            if (entries_[i].pair == pair)
            {
                if (--entries_[i].refs <= 0)
                {
                    for (int j = i; j + 1 < count_; ++j)
                    {
                        entries_[j] = entries_[j + 1];
                    }
                    --count_;
                }
                return;
            }
        }
    }

    int          Count() const noexcept { return count_; }
    const Entry& At(int i) const noexcept { return entries_[i]; }
    void         Clear() noexcept { count_ = 0; }

private:
    std::array<Entry, N> entries_{};
    int                  count_ = 0;
};

/// A Force volume's "inside": entered when not inside yet, left once the game marks it forced out.
constexpr bool ForcedInside(bool current, std::uint8_t forcedOut) noexcept
{
    return !current || forcedOut == 0;
}

// ---- world_actor_reach: widening the items' reach to the seats ----

/// The widening in whole steps past `apart`, the widest distance between player 1 and a seat's member (one seat: the
/// players' distance), with the centre's slack.
inline float WidenBy(float apart, float centreSlack, float step) noexcept
{
    return std::ceil((apart + centreSlack) / step) * step;
}

/// An item near a seat's member: not disabled or pending, `toSeat` (the nearest seat's distance) within its base radius
/// (plus the leave slack once widened).
constexpr bool ItemClose(std::int32_t state, std::int32_t disabled, std::int32_t pending, float toSeat,
                         float baseRadius, bool widenedNow, float leaveSlack) noexcept
{
    return state != disabled && state != pending && toSeat <= baseRadius + (widenedNow ? leaveSlack : 0.0f);
}

/// The game rewrote an item we widened (registered again): its values are the game's.
constexpr bool Rewritten(float writtenRadius, float writtenExtent, float radius, float extent) noexcept
{
    return writtenRadius != radius || writtenExtent != extent;
}

// ---- seat_switch_volumes: a seat's enter and leave of a trigger volume ----

enum class Enter
{
    SkippedOnOff,   // the game's DisableTriggerBox is on (as for player 1)
    AlreadyInside,  // a brush-only volume a seat (this one or another) is inside already: the seat is noted, no call
    Call,           // the volume's enter is called
};

/// `held`: a seat holds the volume already (any seat: a brush-only volume counts no one, so a second seat's enter
/// calls nothing; with one seat, that seat).
constexpr Enter EnterVerdict(bool disableBox, bool disableBoxInOut, bool counts, bool held) noexcept
{
    if (disableBox || disableBoxInOut)
    {
        return Enter::SkippedOnOff;
    }
    return !counts && held ? Enter::AlreadyInside : Enter::Call;
}

enum class Leave
{
    NotEntered,        // the seat was not entered here
    SkippedOnOff,      // DisableTriggerBox_InOut is on: forgotten, not called
    KeptForPlayerOne,  // a brush-only volume another player (player 1 or another seat) is inside: forgotten, not called
    Call,              // the volume's leave is called
};

/// `held`: the leaving seat holds the volume. `otherInside`: player 1 or another seat is inside it too.
constexpr Leave LeaveVerdict(bool held, bool disableBoxInOut, bool counts, bool otherInside) noexcept
{
    if (!held)
    {
        return Leave::NotEntered;
    }
    if (disableBoxInOut)
    {
        return Leave::SkippedOnOff;
    }
    return !counts && otherInside ? Leave::KeptForPlayerOne : Leave::Call;
}

enum class PlayerOneChange
{
    Entered,       // player 1 is inside too: noted
    StillInside,   // player 1 left and the volume still shows someone inside: nothing
    NotReentered,  // DisableTriggerBox is on: not entered again
    Reenter,       // the volume's enter is called again, once, for the seats inside
};

constexpr PlayerOneChange PlayerOneChangeOf(bool enter, bool insideBefore, bool disableBox, bool disableBoxInOut) noexcept
{
    if (enter)
    {
        return PlayerOneChange::Entered;
    }
    if (insideBefore)
    {
        return PlayerOneChange::StillInside;
    }
    return disableBox || disableBoxInOut ? PlayerOneChange::NotReentered : PlayerOneChange::Reenter;
}

// ---- seat_switch_volumes: the seats inside one volume (a mask, bit s for seat s) ----

/// A seat's bit in a volume's mask of the seats inside.
constexpr std::uint8_t VolumeSeatBit(int seat) noexcept
{
    return seat >= 0 && seat < 8 ? static_cast<std::uint8_t>(1u << seat) : std::uint8_t{0};
}

/// Another player than `seat` is inside the volume: player 1, or another seat in `seatsInside`.
constexpr bool OtherInside(std::uint8_t seatsInside, int seat, bool playerOneInside) noexcept
{
    return playerOneInside || (seatsInside & static_cast<std::uint8_t>(~VolumeSeatBit(seat))) != 0;
}

/// A seat's enter: the volume's enter is called unless the on/off skips it or the volume counts no one and a seat is
/// inside already (any seat: then the seat is only noted inside). One seat: the seat itself held it.
constexpr Enter SeatEnterVerdict(bool disableBox, bool disableBoxInOut, bool counts, std::uint8_t seatsInside) noexcept
{
    return EnterVerdict(disableBox, disableBoxInOut, counts, seatsInside != 0);
}

/// A seat's leave: called unless the seat was not entered, the on/off skips it, or the volume counts no one and
/// another player (player 1 or another seat) is inside. One seat: today's leave by player 1's presence.
constexpr Leave SeatLeaveVerdict(std::uint8_t seatsInside, int seat, bool disableBoxInOut, bool counts,
                                 bool playerOneInside) noexcept
{
    const bool held = (seatsInside & VolumeSeatBit(seat)) != 0;
    return LeaveVerdict(held, disableBoxInOut, held && counts, held && OtherInside(seatsInside, seat, playerOneInside));
}

/// The leave calls for a seat that is gone (its leave of every volume, without the on/off test): a counting volume's
/// share of the seat's enters, else one call unless another player is inside.
constexpr std::int32_t LeaveAllCalls(bool counts, std::int32_t seatEnters, std::uint8_t seatsInside, int seat,
                                     bool playerOneInside) noexcept
{
    if (counts)
    {
        return seatEnters;
    }
    return OtherInside(seatsInside, seat, playerOneInside) ? 0 : 1;
}

/// The seat whose trigger's tag the enter called again after player 1's leave uses (once for every seat inside): the
/// lowest seat inside, or -1 with none.
constexpr int ReenterSeat(std::uint8_t seatsInside) noexcept
{
    for (int seat = 1; seat < 8; ++seat)
    {
        if ((seatsInside & VolumeSeatBit(seat)) != 0)
        {
            return seat;
        }
    }
    return -1;
}

// ---- camera_repair: when a non-finite pose is repaired, and when the tries start over ----

/// Follows whether the pose has been finite for long enough that a new NaN starts a new count of tries.
class HealthyTimer
{
public:
    using Clock = std::chrono::steady_clock;

    /// True once the pose has been finite for `healthyFor` (every tick since it came back).
    bool Observe(bool finite, Clock::time_point now, Clock::duration healthyFor) noexcept
    {
        if (!finite)
        {
            since_.reset();
            return false;
        }
        if (!since_)
        {
            since_ = now;
            return false;
        }
        return now - *since_ >= healthyFor;
    }

private:
    std::optional<Clock::time_point> since_;
};

enum class Repair
{
    None,
    Repair,  // a repair runs now
    GiveUp,  // the tries are spent: logged once
};

/// A repair only while the pose is non-finite, outside a follow-up, with a current operator, after the retry gap;
/// then while tries are left, else a give-up (once).
constexpr Repair RepairVerdict(bool finite, bool following, bool opCurrent, bool retryDue, unsigned inARow, unsigned maxInARow,
                               bool gaveUp) noexcept
{
    if (finite || following || !opCurrent || !retryDue)
    {
        return Repair::None;
    }
    if (inARow < maxInARow)
    {
        return Repair::Repair;
    }
    return gaveUp ? Repair::None : Repair::GiveUp;
}

/// The pose floats that are no camera value: bit i for the float at pose offset 4 * i (RE-p2-framing.md §1.3). The
/// location (+0x00), pivot (+0x20) and base point (+0x30) are three floats each, stored with one 16-byte move whose
/// fourth lane is whatever the stack held (UpdateModules, 1.005 0x140D4FC94); the game reads the three floats back.
/// A NaN in a lane is no lost camera: the base point's read -nan for a whole join after a buggy exit while the camera
/// was right, and the game's reset cut the view every half second for nothing (seen in play).
constexpr std::uint32_t kPoseLanes = (1u << 3) | (1u << 11) | (1u << 15);

/// Whether a pose is finite where it counts: `nonFinite` (bit i: the float at pose offset 4 * i is not finite)
/// has no bit outside the lanes.
constexpr bool PoseFinite(std::uint32_t nonFinite) noexcept
{
    return (nonFinite & ~kPoseLanes) == 0;
}

// ---- traversal_camera: the climb move's exit phases and the ladder exit's wait ----

/// The climb move's exit phases: 3, 6, 10 switch the camera off in the Tick; 11, 12 only ask for the exit blend.
constexpr bool ClimbExitState(std::uint8_t state) noexcept
{
    return state == 3 || state == 6 || state == 10 || state == 11 || state == 12;
}

/// The ladder exit's scoped camera off waits for the exit blend, up to `limit` Ticks, while an exit request stands.
constexpr bool LadderExitWaits(bool requestStands, bool blending, bool off, unsigned waited, unsigned limit) noexcept
{
    return requestStands && !blending && !off && waited < limit;
}

// ---- camera_distance and shoulder_scale ----

/// The field camera's distance level, one for every player: the Options level plus the layout's offset, clamped.
constexpr std::int32_t DistanceLevel(std::int32_t field, std::int32_t offset, std::int32_t min, std::int32_t max) noexcept
{
    return std::clamp(field + offset, min, max);
}

/// The shoulder factor: the half's width against the camera's, kept within the minimum and the maximum (only ever
/// softened).
inline float ShoulderScale(float ratio, float minimum, float maximum) noexcept
{
    return std::fmin(std::fmax(ratio, minimum), maximum);
}

}  // namespace lss::world::world_rules
