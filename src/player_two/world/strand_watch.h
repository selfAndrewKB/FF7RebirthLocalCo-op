#pragma once

// When player 2's member counts as stranded off the loaded navmesh (RE-p2-party-leash.md §2.1).
//
// The input is the game's own decision, not the member's speed or animation: each tick says whether
// the game refused (zeroed) at least one of the member's moves because the navmesh under her is not
// loaded. The game only asks when a move was requested, so a player who lets go of the stick produces
// no refusals, and a player pushing the stick while the member runs in place produces one per move.
//
// A strand begins with the first refused move and ends once no move has been refused for longer than
// the grace (the stick let go, or the navmesh came back). Refused falls arrive in bursts about 0.3 s
// apart at some edges (RE-p2-party-leash.md §6c), so the grace spans those gaps; since a refusal only
// happens where her world is not loaded, a longer grace never joins a push she could have walked out
// of. The grace is the mod's; the default hold and cooldown are the game's
// own follow-teleport timings (RE-p2-party-warp.md §2); a caller may shorten the hold. A strand held for
// the hold asks for one warp, and no warp is asked for within the cooldown of the last one. Pure logic;
// the caller supplies the time.

#include <cstdint>

namespace lss::player_two {

enum class StrandEvent : std::uint8_t
{
    None,
    Began,  // the first refused move
    Warp,   // held long enough: the member should be brought back
    Ended,  // no refused move for longer than the grace
};

struct StrandRules
{
    float holdSeconds     = 1.0f;   // the game's follow-teleport hold
    float graceSeconds    = 0.5f;   // a gap between refused moves that still counts as the same strand
    float cooldownSeconds = 2.0f;   // the game's follow-teleport cooldown
};

/// The failed safe-transform checks that count as a strand: only while the game holds the character parked (short of
/// the park, the game return-warps it to its safe spot itself; RE-p2-party-leash.md §6b, §6d).
constexpr std::uint32_t ParkChecks(std::uint32_t failedChecks, bool parked) noexcept
{
    return parked ? failedChecks : 0;
}

class StrandWatch
{
public:
    explicit StrandWatch(StrandRules rules = {}) noexcept : rules_(rules) {}

    /// One tick. `refused`: the game zeroed at least one of the member's moves since the last tick.
    StrandEvent Step(bool refused, float seconds) noexcept;

    /// Forgets the strand and the cooldown (player 2 left, or the member changed).
    void Reset() noexcept { *this = StrandWatch(rules_); }

    bool  Stranded() const noexcept { return stranded_; }
    float Held() const noexcept { return held_; }  // seconds since the strand began

private:
    StrandRules rules_;
    bool        stranded_ = false;
    bool        asked_    = false;  // this strand has asked for its warp
    float       held_     = 0.0f;
    float       gap_      = 0.0f;   // seconds since the last refused move
    float       cooldown_ = 0.0f;
};

}  // namespace lss::player_two
