#pragma once

// The decisions of player 2's audio listener (player_two/feedback/audio_listener_two.h) as pure functions, tested on
// the host (tests/listener_choice_test.cpp). The module keeps the threads, the engine reads and the redirects.

#include <array>

namespace lss::player_two::listener_choice {

/// FQuat::RotateVector, as the device's push computes it for listener [0] (two cross products). `q` is x, y, z, w.
inline std::array<float, 3> RotateVector(const float* q, std::array<float, 3> v) noexcept
{
    const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                        2.0f * (q[0] * v[1] - q[1] * v[0])};
    return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]), v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
            v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

/// The nearer listener for a sound: at first sight the nearer one (a tie goes to player 1); a sound that already has
/// one keeps it until the other is nearer by the hand-over factor, so a sound between the players does not flip its
/// pan every SEAD frame. True: player 2's.
constexpr bool ChooseNearer(bool known, bool wasTwo, float toOne, float toTwo, float handOver) noexcept
{
    if (!known)
    {
        return toTwo < toOne;
    }
    if (wasTwo ? toOne < toTwo * handOver : toTwo < toOne * handOver)
    {
        return !wasTwo;
    }
    return wasTwo;
}

/// The AutoSe clip takes player 2's listener when the character is nearer to it than to player 1's.
constexpr bool NearerForClip(float toTwo, float toOne) noexcept
{
    return toTwo < toOne;
}

/// The footstep LOD: the game's level from player 1's listener is kept when it is the finest already (or no filter);
/// otherwise player 2's level replaces it only when it is lower.
constexpr bool LodKeepsFirst(int one) noexcept
{
    return one <= 0;
}
constexpr bool LodLowered(int one, int other) noexcept
{
    return other < one;
}

/// The game's audibility test from player 2's listener: magnification x range > the distance from it (a sound the
/// chosen listener culled that player 2 would hear).
constexpr bool MissedByTwo(float range, float magnify, float toTwo) noexcept
{
    return range > 0.0f && magnify * range > toTwo;
}

}  // namespace lss::player_two::listener_choice
