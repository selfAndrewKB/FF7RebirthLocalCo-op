#pragma once

// The HUD modules' pure decisions (hud_placement, search_pins, marker_timers, minimap, shared_slot, target_views,
// enemy_gauges), tested on the host (tests/hud_rules_test.cpp). The modules keep the engine reads, the passes and
// the log.

#include "hud/overlay_pass.h"

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

/// The compass: whoever had an active point while it was open; while it closes nobody does (kept); a new one starts
/// as both players' (bit 1 player 1, bit 2 player 2).
constexpr unsigned CompassOwners(bool compassOpen, bool activeOne, bool activeTwo, unsigned previous) noexcept
{
    const unsigned active = (activeOne ? 1u : 0u) | (activeTwo ? 2u : 0u);
    return !compassOpen ? 3u : active != 0 ? active : previous;
}

/// A pin's owner by who holds it: 0 player 1, 1 player 2, -1 neither (a released pin, still animating out: the
/// caller looks it up among the pins it knew).
constexpr int PinOwnerOf(bool holdsOne, bool holdsTwo) noexcept
{
    return holdsOne ? 0 : holdsTwo ? 1 : -1;
}

// ---- marker_timers: each player's own arrow timers ----

struct Timer
{
    std::uint64_t key = 0;  // the element's FWeakObjectPtr, as stored
    float         t   = 0.0f;
};

/// Player 1's timers set aside during player 2's run, player 2's own kept between frames. The vectors may throw on
/// growth; the caller's try wraps a whole swap.
class TimerSwap
{
public:
    /// Player 2's run starts.
    void BeginIn() { one_.clear(); }

    /// Records player 1's timer for `key` and gives player 2's: 0 for an arrow player 2's run has not seen.
    float In(std::uint64_t key, float mine)
    {
        one_.push_back({key, mine});
        const Timer* theirs = Find(two_, key);
        return theirs != nullptr ? theirs->t : 0.0f;
    }

    /// Player 2's run ends.
    void BeginOut() { seen_.clear(); }

    /// Records player 2's timer for `key` and gives player 1's back, if the element was there before player 2's run
    /// (an element new in player 2's run keeps what it has).
    std::optional<float> Out(std::uint64_t key, float theirs)
    {
        seen_.push_back({key, theirs});
        const Timer* mine = Find(one_, key);
        return mine != nullptr ? std::optional<float>(mine->t) : std::nullopt;
    }

    /// Player 2's own are what this run saw (arrows no longer in the list are dropped); player 1's are back in place.
    void EndOut() noexcept
    {
        two_.swap(seen_);
        one_.clear();
    }

    /// A swap that could not finish: player 1's timers are then left in place.
    void Abandon() noexcept { one_.clear(); }

    void Forget() noexcept
    {
        one_.clear();
        two_.clear();
    }

    std::size_t TwoCount() const noexcept { return two_.size(); }

private:
    static const Timer* Find(const std::vector<Timer>& timers, std::uint64_t key) noexcept
    {
        const auto found =
            std::find_if(timers.begin(), timers.end(), [key](const Timer& timer) { return timer.key == key; });
        return found != timers.end() ? &*found : nullptr;
    }

    std::vector<Timer> one_;   // set aside during player 2's run
    std::vector<Timer> two_;   // player 2's own, between frames
    std::vector<Timer> seen_;  // scratch, reused
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

// ---- shared_slot: player 2's entry beside the game's in one menu-renderer slot ----

enum class SlotAction
{
    Acquire,         // the game's own acquire
    AcquireAndHold,  // player 2's pass: the slot acquired is kept for player 1's pass
    TakeBack,        // player 1's pass: the held slot instead of a second acquire
};

/// `active`: this slot takes part in player 2's pass (its widget paints for player 2 this frame).
constexpr SlotAction SlotActionFor(OverlayPass pass, bool active, bool held) noexcept
{
    if (pass == OverlayPass::PlayerOne && held)
    {
        return SlotAction::TakeBack;
    }
    if (pass == OverlayPass::PlayerTwo && active && !held)
    {
        return SlotAction::AcquireAndHold;
    }
    return SlotAction::Acquire;
}

/// The entry added to the held slot in player 2's pass is player 2's: it gets player 2's name, and its submit is left
/// to player 1's pass (which submits the slot with both entries).
constexpr bool PlayerTwoEntry(OverlayPass pass, bool heldSlot) noexcept
{
    return pass == OverlayPass::PlayerTwo && heldSlot;
}

// ---- target_views: the gauge entries each pass takes out ----

/// The pass that hides a gauge entry: player 2's pass (1) takes out player 1's only, player 1's pass (0) player 2's
/// only; -1 for an entry both or neither show (the caller still leaves a shared source alone).
constexpr int OtherGaugePass(bool inPlayerOnes, bool inPlayerTwos) noexcept
{
    const bool onlyOne = inPlayerOnes && !inPlayerTwos;
    const bool onlyTwo = inPlayerTwos && !inPlayerOnes;
    return onlyOne ? 1 : onlyTwo ? 0 : -1;
}

// ---- enemy_gauges: player 2's placement of each gauge ----

struct GaugeClass
{
    bool own         = false;  // player 2's own target: two offset steps
    bool main        = false;  // player 2's main target
    bool caseC       = false;  // the game's targeted-without-leader-clamp case: no leader scope
    bool target      = false;  // placed as a target
    int  steps       = 1;      // offset steps per pass
    bool leaderScope = false;  // placed inside player 2's leader scope
};

constexpr GaugeClass ClassifyGauge(std::uintptr_t source, std::uintptr_t twoOwn, std::uintptr_t twoMain, bool targeted,
                                   bool skipLeaderClamp, bool forced) noexcept
{
    GaugeClass gauge;
    gauge.own         = source != 0 && source == twoOwn;
    gauge.main        = source != 0 && source == twoMain;
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
