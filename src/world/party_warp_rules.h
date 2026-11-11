#pragma once

// A player's ask to bring their character back beside player 1's (world/nav_strand.h, from L3 clicked twice):
// the double click and the "already beside" refusal as pure rules. The warp itself, and every other refusal (a menu or
// a scene, a ride, a battle, a character in no follow state), is the game's own follow warp's (world/party_warp.h).
// Host-tested (tests/party_warp_rules_test.cpp).

#include <cstdint>

namespace lss::world::party_warp_rules {

/// Two presses within this many milliseconds are one double click (the game has no pad double-click rule of its own).
inline constexpr std::uint64_t kDoubleClickMs = 400;

struct DoubleClick
{
    std::uint64_t lastPress = 0;  // the first press of a pair (0: none waiting)

    /// A press at `now` (milliseconds): true on the second press within kDoubleClickMs of the first; the pair is then
    /// spent. A press later than that starts a new pair.
    constexpr bool Press(std::uint64_t now) noexcept
    {
        if (lastPress != 0 && now - lastPress <= kDoubleClickMs)
        {
            lastPress = 0;
            return true;
        }
        lastPress = now;
        return false;
    }
};

// Beside, in the ground plane: wider than the follow warp's own landing (it puts a follower on its follow spot, 4 to
// 8 m from the leader as the ground allows), so a second double click after a warp moves nothing.
inline constexpr float kNearCm  = 1000.0f;
inline constexpr float kLevelCm = 150.0f;  // and in height: a floor above or below is not beside

/// The character already stands beside player 1's: a double click there moves nothing.
constexpr bool Beside(float flatCm, float heightCm) noexcept
{
    return flatCm < kNearCm && heightCm < kLevelCm && heightCm > -kLevelCm;
}

}  // namespace lss::world::party_warp_rules
