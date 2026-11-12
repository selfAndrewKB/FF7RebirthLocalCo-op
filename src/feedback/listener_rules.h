#pragma once

// The sound listeners' decisions as pure functions (feedback/sound_listeners.h), tested on the host
// (tests/listener_rules_test.cpp). The module keeps the threads, the engine reads and the redirects.
//
// The nearest-of-N choice holds a listener only while it is within the sound's range (a hold kept out of range leaves a
// sound near another player silent), and the frame-stamped choice table carries a probe (one entry per slot would let
// two colliding sounds trade choices between test and update). The gates take the nearest listener: the AutoSe getter
// has no character argument, so the clip and the rustle measure take the nearest player's place instead of asking the
// game once per player. The LOD rules are this game's.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::feedback::listener_rules {

using Vec3 = std::array<float, 3>;

inline float Distance(const Vec3& a, const Vec3& b) noexcept
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

/// SEAD's audibility compare: a sound with a positive range is heard from a listener closer than that range (strictly,
/// as the game's `distance < range`). A moving sound's range is the object's times the listener's magnification.
constexpr bool Within(float range, float distance) noexcept
{
    return range > 0.0f && distance < range;
}

/// FQuat::RotateVector (two cross products), as the SEAD device's push turns the engine's listener [0]. `q` is x, y, z,
/// w.
inline Vec3 RotateVector(const float* q, const Vec3& v) noexcept
{
    const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                        2.0f * (q[0] * v[1] - q[1] * v[0])};
    return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]), v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
            v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

/// The front and up the push sends SEAD for an engine listener's rotation: front = Q.X, up = -(Q.Z).
inline Vec3 SeadFront(const float* q) noexcept
{
    return RotateVector(q, {1.0f, 0.0f, 0.0f});
}
inline Vec3 SeadUp(const float* q) noexcept
{
    const Vec3 up = RotateVector(q, {0.0f, 0.0f, 1.0f});
    return {-up[0], -up[1], -up[2]};
}

/// A sound with no listener chosen yet.
inline constexpr int kNoChoice = -1;
/// A sound changes listener once another is this much nearer (20 %).
inline constexpr float kHandOver = 0.8f;

/// The nearest listener (index 0 player 1's; a listener that is off has a distance that is not finite), a tie going to
/// the lower index, so player 1 first; player 1's when none is on. AutoSe's gates: the clip and the rustle measure
/// read the nearest player's place.
inline int Nearest(std::span<const float> distances) noexcept
{
    int best = kNoChoice;
    for (int i = 0; i < static_cast<int>(distances.size()); ++i)
    {
        if (std::isfinite(distances[i]) && (best == kNoChoice || distances[i] < distances[best]))
        {
            best = i;
        }
    }
    return best == kNoChoice ? 0 : best;
}

/// The listener a sound is measured from, by its distance to each (as Nearest) and its audible range (0 or less: not
/// known yet, as before a moving sound's first test). A new sound (kNoChoice) or one whose range is not known takes the
/// nearest. A sound that has one keeps it while that listener is within its range until another is nearer than
/// kHandOver times its distance, so a sound between two players does not flip its pan every SEAD frame; it leaves a
/// listener that went off or out of its range at once, for the nearest (which hears it if any listener does). A choice
/// inherited at a reused address is therefore harmless.
inline int ChooseNearest(int current, std::span<const float> distances, float range) noexcept
{
    const int count = static_cast<int>(distances.size());
    const int best  = Nearest(distances);
    if (current < 0 || current >= count || !std::isfinite(distances[current]) || !Within(range, distances[current]))
    {
        return best;
    }
    return distances[best] < distances[current] * kHandOver ? best : current;
}

/// The footstep LOD (gate B): the game's level from player 1's listener is kept when it is the finest already (or no
/// filter); otherwise a seat's level replaces it only when it is lower.
constexpr bool LodKeepsFirst(int one) noexcept
{
    return one <= 0;
}
constexpr bool LodLowered(int kept, int other) noexcept
{
    return other < kept;
}

/// Each sound's listener, kept from its test to its update (the same SEAD frame: the tests run in one loop, the updates
/// in a second) and to its next test (the hold). Keyed by the sound object's address with a short linear probe; an
/// entry not refreshed in the frame before is free, so a freed sound's entry lapses by itself. One thread (SEAD's).
template <std::size_t Bits, std::size_t Probe>
class ChoiceTable
{
    static_assert(Bits >= 1 && Bits < 32 && Probe >= 1 && Probe <= (std::size_t{1} << Bits), "a probe within the table");

public:
    /// The listener the sound's test chose in `frame` or the frame before (kNoChoice if none): the one it holds.
    int Held(const void* object, std::uint32_t frame) const noexcept
    {
        const Entry* entry = Find(object, frame);
        return entry != nullptr ? entry->listener : kNoChoice;
    }
    /// The listener the sound's test chose in `frame` (kNoChoice if its test did not run in it): its update's.
    int ChosenIn(const void* object, std::uint32_t frame) const noexcept
    {
        const Entry* entry = Find(object, frame);
        return entry != nullptr && entry->frame == frame ? entry->listener : kNoChoice;
    }
    /// Records the sound's choice in `frame`. False, recording nothing, when its probe holds no free entry (the sound
    /// is then player 1's, its test's and its update's alike).
    bool Record(const void* object, int listener, std::uint32_t frame) noexcept
    {
        Entry* free = nullptr;
        for (std::size_t step = 0; step < Probe; ++step)
        {
            Entry& entry = entries_[(Home(object) + step) & kMask];
            if (entry.object == object)
            {
                entry = {object, listener, frame};
                return true;
            }
            if (free == nullptr && !Fresh(entry, frame))
            {
                free = &entry;
            }
        }
        if (free == nullptr)
        {
            return false;
        }
        *free = {object, listener, frame};
        return true;
    }

private:
    static constexpr std::size_t kMask = (std::size_t{1} << Bits) - 1;

    struct Entry
    {
        const void*   object   = nullptr;
        int           listener = kNoChoice;
        std::uint32_t frame    = 0;
    };

    static std::size_t Home(const void* object) noexcept
    {
        const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object) >> 4);
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - Bits));
    }
    static bool Fresh(const Entry& entry, std::uint32_t frame) noexcept
    {
        return entry.object != nullptr && frame - entry.frame <= 1u;
    }
    const Entry* Find(const void* object, std::uint32_t frame) const noexcept
    {
        for (std::size_t step = 0; step < Probe; ++step)
        {
            const Entry& entry = entries_[(Home(object) + step) & kMask];
            if (entry.object == object)
            {
                return Fresh(entry, frame) ? &entry : nullptr;
            }
        }
        return nullptr;
    }

    std::array<Entry, std::size_t{1} << Bits> entries_{};
};

}  // namespace lss::feedback::listener_rules
