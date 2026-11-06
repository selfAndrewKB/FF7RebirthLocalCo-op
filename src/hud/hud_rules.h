#pragma once

// The HUD modules' pure decisions (hud_placement, search_pins, marker_timers, minimap, target_views, enemy_gauges),
// tested on the host (tests/hud_rules_test.cpp; the shared slot's are hud/passes/overlay_pass.h's, tested there too).
// The modules keep the engine reads, the passes and the log.

#include "players/seats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lss::hud::hud_rules {

// ---- hud_placement: a widget the game keeps setting back ----

/// Re-placements on (nearly) consecutive ticks: a gap of more than two ticks starts the count over.
constexpr std::uint32_t StreakAfter(std::uint32_t ticks, std::uint32_t lastPlacedTick, std::uint32_t streak) noexcept
{
    return ticks - lastPlacedTick <= 2 ? streak + 1 : 0;
}

enum class Restore
{
    Skip,      // never changed, left alone, or gone: nothing to set back
    GamesOwn,  // the game set it after the last tick's check: that layout is the game's own
    Restore,   // the placed layout is still in place: the original goes back
};

/// `nowIsApplied()` reads the widget only when the verdict needs it.
template <class NowFn>
Restore RestoreVerdict(bool managed, bool unchanged, bool alive, NowFn&& nowIsApplied) noexcept
{
    if (!managed || unchanged || !alive)
    {
        return Restore::Skip;
    }
    return nowIsApplied() ? Restore::Restore : Restore::GamesOwn;
}

// ---- search_pins: who owns the compass and each pin ----

/// Every player's bit (bit `seat`, player 1 bit 0): the owners of a compass nobody has claimed yet.
inline constexpr unsigned kEveryPlayer = 0b111;
static_assert(players::kMaxPlayers == 3, "kEveryPlayer has one bit per player");

/// The compass: whoever had an active point while it was open (bit `seat` each); while it closes nobody does (kept); a
/// new one starts as every player's.
constexpr unsigned CompassOwners(bool compassOpen, unsigned active, unsigned previous) noexcept
{
    return !compassOpen ? kEveryPlayer : active != 0 ? active : previous;
}

/// A pin's owner by who holds it (bit `seat` each): the lowest seat holding it, player 1 first; -1 nobody (a released
/// pin, still animating out: the caller looks it up among the pins it knew).
constexpr int PinOwnerOf(unsigned holders) noexcept
{
    for (int seat = 0; seat < 32; ++seat)
    {
        if ((holders & (1u << seat)) != 0)
        {
            return seat;
        }
    }
    return -1;
}

// ---- marker_timers: each player's own arrow timers ----

struct Timer
{
    std::uint64_t key = 0;  // the element's FWeakObjectPtr, as stored
    float         t   = 0.0f;
};

/// Player 1's timers set aside during a seat's run, the seat's own kept between frames (one swap per seat). The
/// vectors may throw on growth; the caller's try wraps a whole swap.
class TimerSwap
{
public:
    /// The seat's run starts.
    void BeginIn() { one_.clear(); }

    /// Records player 1's timer for `key` and gives the seat's: 0 for an arrow the seat's run has not seen.
    float In(std::uint64_t key, float mine)
    {
        one_.push_back({key, mine});
        const Timer* theirs = Find(seatTimers_, key);
        return theirs != nullptr ? theirs->t : 0.0f;
    }

    /// The seat's run ends.
    void BeginOut() { seen_.clear(); }

    /// Records the seat's timer for `key` and gives player 1's back, if the element was there before the seat's run
    /// (an element new in the seat's run keeps what it has).
    std::optional<float> Out(std::uint64_t key, float theirs)
    {
        seen_.push_back({key, theirs});
        const Timer* mine = Find(one_, key);
        return mine != nullptr ? std::optional<float>(mine->t) : std::nullopt;
    }

    /// The seat's own are what this run saw (arrows no longer in the list are dropped); player 1's are back in place.
    void EndOut() noexcept
    {
        seatTimers_.swap(seen_);
        one_.clear();
    }

    /// A swap that could not finish: player 1's timers are then left in place.
    void Abandon() noexcept { one_.clear(); }

    void Forget() noexcept
    {
        one_.clear();
        seatTimers_.clear();
    }

    std::size_t SeatTimerCount() const noexcept { return seatTimers_.size(); }

private:
    static const Timer* Find(const std::vector<Timer>& timers, std::uint64_t key) noexcept
    {
        const auto found =
            std::find_if(timers.begin(), timers.end(), [key](const Timer& timer) { return timer.key == key; });
        return found != timers.end() ? &*found : nullptr;
    }

    std::vector<Timer> one_;         // set aside during the seat's run
    std::vector<Timer> seatTimers_;  // the seat's own, between frames
    std::vector<Timer> seen_;        // scratch, reused
};

// ---- minimap: a map piece's unload, asked for both players ----

enum class Unload
{
    GamesOwn,      // the game's answer stands: kept by it, or the other player's centre is not known yet
    KeptForOther,  // the game would drop it, but the other player's map shows it
    Dropped,       // out of range of both
};

/// `dropForOther()` asks the game's test for the other player's centre, only when the verdict needs it.
template <class OtherFn>
Unload UnloadVerdict(bool drop, bool otherCentreKnown, OtherFn&& dropForOther) noexcept
{
    if (!drop || !otherCentreKnown)
    {
        return Unload::GamesOwn;
    }
    return dropForOther() ? Unload::Dropped : Unload::KeptForOther;
}

// ---- target_views: the gauge entries each pass takes out ----

/// Whether `pass`'s view (its seat, 0 player 1's) takes a gauge entry out: the entry is some player's target (bit
/// `seat` each in `holders`) but not this view's player's. An entry nobody targets is the game's to show (the caller
/// still leaves a shared source alone).
constexpr bool GaugeHiddenIn(unsigned holders, int pass) noexcept
{
    return holders != 0 && pass >= 0 && (holders & (1u << pass)) == 0;
}

// ---- enemy_gauges: a seat's placement of each gauge ----

struct GaugeClass
{
    bool own         = false;  // the seat's own target: two offset steps
    bool main        = false;  // the seat's main target
    bool caseC       = false;  // the game's targeted-without-leader-clamp case: no leader scope
    bool target      = false;  // placed as a target
    int  steps       = 1;      // offset steps per pass
    bool leaderScope = false;  // placed inside the seat's leader scope
};

constexpr GaugeClass ClassifyGauge(std::uintptr_t source, std::uintptr_t seatOwn, std::uintptr_t seatMain,
                                   bool targeted, bool skipLeaderClamp, bool forced) noexcept
{
    GaugeClass gauge;
    gauge.own         = source != 0 && source == seatOwn;
    gauge.main        = source != 0 && source == seatMain;
    gauge.caseC       = targeted && skipLeaderClamp;
    gauge.target      = gauge.own || gauge.main || gauge.caseC || forced;
    gauge.steps       = gauge.own ? 2 : 1;
    gauge.leaderScope = gauge.target && !gauge.caseC;
    return gauge;
}

/// The mod's targeted rule for player 1's view, checked against the game's own choice (logged when they disagree).
constexpr bool PlayerOneTargetRule(std::uintptr_t source, std::uintptr_t oneA, std::uintptr_t oneB, bool targeted,
                                   bool skipLeaderClamp, bool forced) noexcept
{
    return (source != 0 && (source == oneA || source == oneB)) || (targeted && skipLeaderClamp) || forced;
}

}  // namespace lss::hud::hud_rules
